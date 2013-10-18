//------------------------------------------------------------------------------------------------------------------
// OSWIN Multiple TX to emoncms
// For the OSWIN Internet Gateway using WIZ820io ethernet http://zorg.org/oswin/
// Receives data from multiple TinyTX sensors and/or emonTX and uploads to an emoncms server.
//
// Supports RFM12B, Ciseco XRF or other Xbee type radios or OOK/ASK receiver
// With RFM12B optionally gets NTP time once an hour and transmits for receipt by remote displays
// ACKs are supported with RFM12B only at this time
// If using RFM12B see README for required modifications to Jeelib library for use with ATmega1284P
//
// By Nathan Chantrell. http://zorg.org/
// Licenced under the Creative Commons Attribution-ShareAlike 3.0 Unported (CC BY-SA 3.0) licence:
// http://creativecommons.org/licenses/by-sa/3.0/
//------------------------------------------------------------------------------------------------------------------

// Options
#define DEBUG                // uncomment for serial output (57600 baud)
#define USE_RFM12B           // comment out to disable RFM12B use
//#define USE_XRF            // comment out to disable XRF/Xbee use
//#define USE_OOK            // comment out to disable OOK receiver use
#define USE_NTP              // Comment out to disable NTP transmit function (only with RFM12B)

#include <SPI.h>             
#include <avr/wdt.h>
#include <Ethernet52.h>      // https://bitbucket.org/homehack/ethernet52/src For WIZ820io ethernet
#include <EthernetUdp.h>
#ifdef USE_RFM12B
 #include <JeeLib.h>         // https://github.com/jcw/jeelib See README for mod to use with ATmega1284P
#endif
#ifdef USE_XRF
 #include <EasyTransfer.h>   // https://github.com/madsci1016/Arduino-EasyTransfer/tree/master/EasyTransfer
#endif 
#ifdef USE_OOK
 #include <MANCHESTER.h>      // https://github.com/mchr3k/arduino-libs-manchester See README for mod to run at 12MHz
#endif
 
// RFM12B settings
#define MYNODE 30            // node ID 30 reserved for base station
#define freq RF12_433MHZ     // frequency
#define group 210            // network group 

// OOK Receiver settings
#define OokRxPin 20  // Pin OOK Receivers data pin is connected to
#define OokPower 18  // Pin OOK Receivers power pin is connected to
#define OokGnd 21    // Pin OOK Receivers GND pin is connected to

// emoncms settings, change these settings to match your own setup
IPAddress server(192,168,0,21);              // IP address of emoncms server
#define HOSTNAME "server.local"              // Hostname of emoncms server
#define EMONCMS "emoncms"                    // location of emoncms on server, blank if at root
#define APIKEY  "xxxxxxxxxxxxxxxxxxxxxxxx"   // API write key 

// NTP Stuff *Currently only supported with RFM12B*
// Retrieves time from NTP server and transmits for receipt by remote displays
#define NTP_PERIOD 60                     // How often to get & transmit the time in minutes
#define UDP_PORT 8888                     // Local port to listen for UDP packets
#define NTP_PACKET_SIZE 48                // NTP time stamp is in the first 48 bytes of the message
IPAddress timeServer(216, 171, 120, 36);  // time.nist.gov NTP server
byte packetBuffer[ NTP_PACKET_SIZE];      // Buffer to hold incoming and outgoing packets 
EthernetUDP Udp;                          // A UDP instance to let us send and receive packets over UDP
unsigned long timeTX = -NTP_PERIOD*60000; // for time transmit function

// RGB Status LED
#define redLed 31     // OSWIN Red LED on Pin 31 / PD7 / PWM capable - used to indicate setup or error
#define greenLed 30   // OSWIN Green LED on Pin 30 / PD4 / PWM capable - used to indicate everything ok
#define blueLed 7     // OSWIN Blue LED on Pin 7 / PB3 / PWM capable - used to indicate data received

#ifdef USE_RFM12B
// Received payload structure for RFM12B
typedef struct {
  int data1;		 // received data 1
  int supplyV;           // voltage
  int data2;		 // received data 2
} Payload;
Payload rx; 

// The RF12 data payload to be sent, used to send server time to GLCD display
typedef struct {
  int hour, mins, sec;  // time
} PayloadOut;
PayloadOut ntp; 
#endif

#ifdef USE_XRF
 EasyTransfer ET;  // Create SoftEasyTransfer object

// Received payload structure for XRF
  typedef struct {
  	  byte nodeID;	// Sensor Node ID
   	  int temp;	// Temperature reading
  	  int supplyV;	// Supply voltage
 } PayloadXrf;

 PayloadXrf xrfrx;
#endif

#ifdef USE_OOK
// Received payload structure for OOK
 typedef struct {
          int nodeID;      // sensor node ID
          int data;        // sensor value
          int supplyV;     // tx voltage
 } PayloadOok;
 PayloadOok ookrx;

unsigned char databuf[6];
unsigned char* data = databuf;
#endif

// PacketBuffer class used to generate the json string that is send via ethernet - JeeLabs
class PacketBuffer : public Print {
public:
    PacketBuffer () : fill (0) {}
    const char* buffer() { return buf; }
    byte length() { return fill; }
    void reset() { memset(buf,NULL,sizeof(buf)); fill = 0; }
    virtual size_t write(uint8_t ch)
        { if (fill < sizeof buf) buf[fill++] = ch; }  
    byte fill;
    char buf[150];
    private:
};
PacketBuffer str;

// Ethernet
  byte mac[] = {  0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };  // ethernet mac address, must be unique on the LAN
  byte ip[] = {192, 168, 0, 178 };                       // Backup static IP to be used if DHCP fails
  EthernetClient client;

// Flow control varaiables
  int dataReady=0;                         // is set to 1 when there is data ready to be sent

int rxnodeID; // The incoming node we pass to emoncms

//--------------------------------------------------------------------
// Setup
//--------------------------------------------------------------------

void setup () {
  
  pinMode(redLed, OUTPUT);       // Set red LED Pin as output
  pinMode(greenLed, OUTPUT);     // Set red LED Pin as output
  pinMode(blueLed, OUTPUT);      // Set red LED Pin as output
  digitalWrite(redLed,HIGH);     // Turn red LED on during setup

  #ifdef USE_XRF
    Serial1.begin(9600);         // Serial 1 for communication with the XRF/Xbee
    ET.begin(details(xrfrx), &Serial1);
  #endif
  
  #ifdef USE_OOK
     pinMode(OokGnd, OUTPUT);     // Set OOK GND Pin as output
     pinMode(OokPower, OUTPUT);   // Set OOK Power Pin as output
     digitalWrite(OokGnd,LOW);    // Set OOK GND Pin Low
     digitalWrite(OokPower,HIGH); // Set OOK Power Pin High
     MANRX_SetRxPin(OokRxPin);    // Set rx pin
     MANRX_SetupReceive();        // Prepare interrupts
     MANRX_BeginReceive();        // Begin receiving data
  #endif
  
  #ifdef DEBUG
    Serial.begin(57600);
    Serial.println("-------------------------------------------------");
    Serial.println("OSWIN 2.0 Multiple TX to emoncms");
    #ifdef USE_RFM12B
     Serial.print("RFM12B support enabled ");
     Serial.print("Node: "); Serial.print(MYNODE); 
     Serial.print(" Freq: "); 
      if (freq == RF12_433MHZ) Serial.print("433 MHz");
      if (freq == RF12_868MHZ) Serial.print("868 MHz");
      if (freq == RF12_915MHZ) Serial.print("915 MHz");  
     Serial.print(" Network group: "); Serial.println(group);
     #ifdef USE_NTP
      Serial.println("NTP time transmit enabled");
     #endif
    #endif
    #ifdef USE_XRF
     Serial.println("XRF/Xbee support enabled");
    #endif
    #ifdef USE_OOK
     Serial.println("OOK support enabled");
    #endif
  #endif
 
// Manually configure ip address if DHCP fails
  if (Ethernet.begin(mac) == 0) {
  #ifdef DEBUG
    Serial.println("DHCP failed");
  #endif
    Ethernet.begin(mac,ip);  
  }

// print the IP address:
  #ifdef DEBUG
  Serial.print("IP:  ");
  for (byte thisByte = 0; thisByte < 4; thisByte++) {
   Serial.print(Ethernet.localIP()[thisByte], DEC); 
   Serial.print(".");
  }
  Serial.println();
  Serial.println("-------------------------------------------------");
  #endif

  #ifdef USE_RFM12B
  rf12_initialize(MYNODE,freq,group);     // initialise the RFM12B
  #endif
  
  #ifdef USE_NTP
  Udp.begin(UDP_PORT);                    // Start UDP for NTP client
  #endif
  
  digitalWrite(redLed,LOW);               // Turn red LED off to indicate setup has finished

  delay(1000);
  
  wdt_enable(WDTO_8S);                    // Enable the watchdog timer with an 8 second timeout
    
}

//--------------------------------------------------------------------
// Loop
//--------------------------------------------------------------------

void loop () {
  
  wdt_reset(); // Reset the watchdog timer

  analogWrite(greenLed,5);            // Turn green LED on dim

#ifdef USE_RFM12B
  #ifdef USE_NTP
  if ((millis()-timeTX)>(NTP_PERIOD*60000)){    // Send NTP time
    getTime();
  } 
  #endif
#endif

//--------------------------------------------------------------------  
// On data receieved from rf12
//--------------------------------------------------------------------

#ifdef USE_RFM12B
  if (dataReady==0 && rf12_recvDone() && rf12_crc == 0 && (rf12_hdr & RF12_HDR_CTL) == 0) 
  {

   int nodeID = rf12_hdr & 0x1F;          // extract node ID from received packet
   rx=*(Payload*) rf12_data;              // Get the payload
   digitalWrite(greenLed,LOW);            // Turn green LED off
   analogWrite(blueLed,100);              // Turn blue LED on to indicate data to send 

   #ifdef DEBUG
    Serial.println();
    Serial.print("Data received from RFM12B Node ");
    Serial.println(nodeID);
   #endif
   
   rxnodeID = nodeID;
   
   if (RF12_WANTS_ACK) {                  // Send ACK if requested
     #ifdef DEBUG
      Serial.println("-> ack sent");
     #endif
     rf12_sendStart(RF12_ACK_REPLY, 0, 0);
   }

// JSON creation: format: {key1:value1,key2:value2} and so on
    
   str.reset();                           // Reset json string     
   
   str.print("data1:");
   str.print(rx.data1);                   // Add reading 1
   
   str.print(",data2:"); 
   str.print(rx.data2);                   // Add reading 2
   
   str.print(",v:");
   str.print(rx.supplyV);                 // Add tx battery voltage reading

   str.print("}\0");

   dataReady = 1;                         // Ok, data is ready

  }

#endif

#ifdef USE_XRF
  if(ET.receiveData()){

   digitalWrite(greenLed,LOW);            // Turn green LED off
   analogWrite(blueLed,100);              // Turn blue LED on to indicate data to send 

    #ifdef DEBUG
     Serial.println();
     Serial.print("Data received from SRF/XRF Node ");
     Serial.println(xrfrx.nodeID);
    #endif
    
   rxnodeID = xrfrx.nodeID;
   
// JSON creation: format: {key1:value1,key2:value2} and so on
    
   str.reset();                           // Reset json string     
   
   str.print("data1:");
   str.print(xrfrx.temp);                 // Add reading 1
 
   str.print(",v:");
   str.print(xrfrx.supplyV);              // Add tx battery voltage reading

   str.print("}\0");

   dataReady = 1;                         // Ok, data is ready

 }
#endif

#ifdef USE_OOK
 if (MANRX_ReceiveComplete()) {      
   
    unsigned char receivedSize;
    unsigned char* msgData;
    MANRX_GetMessageBytes(&receivedSize, &msgData);
    MANRX_BeginReceiveBytes(6, data);
  
   ookrx = *(PayloadOok*) data;
  
   if (ookrx.nodeID != 0) {
     
   digitalWrite(greenLed,LOW);            // Turn green LED off
   analogWrite(blueLed,100);              // Turn blue LED on to indicate data to send 

    #ifdef DEBUG
     Serial.println();
     Serial.print("Data received from OOK Node ");
     Serial.println(ookrx.nodeID);
    #endif    
    
   rxnodeID = ookrx.nodeID;
     
  // JSON creation: format: {key1:value1,key2:value2} and so on
    
   str.reset();                           // Reset json string     
   
   str.print("data1:");
   str.print(ookrx.data);                 // Add reading 1
   
   str.print(",v:");
   str.print(ookrx.supplyV);              // Add tx battery voltage reading

   str.print("}\0");

   dataReady = 1;                         // Ok, data is ready
   }
   
 }
#endif

 
//--------------------------------------------------------------------
// Send the data
//--------------------------------------------------------------------
 
  if (!client.connected() && dataReady==1) {     // If not connected and data is ready: send data

   #ifdef DEBUG
    Serial.println("Connecting to server");
   #endif
   
   wdt_reset(); // Reset the watchdog timer
    
   // Send the data  
   if (client.connect(server, 80)) {     // Connect to server

    if (client.connected()) {            // If connected send data
    
     #ifdef DEBUG
      Serial.print("Sending to emoncms: ");
      Serial.println(str.buf);
     #endif
   
     // The HTTP GET request   
     client.print("GET "); client.print("/"EMONCMS"/api/post.json?apikey="APIKEY"&node="); client.print(rxnodeID); client.print("&json="); client.print(str.buf);       
     client.println(" HTTP/1.1");
     client.print("Host: ");
     client.println(HOSTNAME);
     client.print("User-Agent: OSWIN 2.0");
     client.println("\r\n");
 
     #ifdef DEBUG
      Serial.println("Sent OK");
     #endif

     digitalWrite(redLed,LOW);           // Turn red LED OFF to clear any error
     digitalWrite(blueLed,LOW);          // Turn blue LED OFF to indicate data sent

     dataReady = 0;                      // reset data ready flag

    } 
    else { // If not connected retry on next loop
     digitalWrite(greenLed,LOW); digitalWrite(redLed,HIGH);   // Turn green LED OFF and red LED ON to indicate error
     #ifdef DEBUG
     Serial.println("Retrying... ");
     #endif
    }


   } 
   else {   // We didn't get a connection to the server, will retry on next loop
     digitalWrite(greenLed,LOW); digitalWrite(redLed,HIGH);   // Turn green LED OFF and red LED ON to indicate error
     #ifdef DEBUG
     Serial.print("ERROR: Connection failed to ");
     Serial.print(server);
     Serial.println(", will retry");
     #endif
   }

   client.stop();  // Disconnect
   #ifdef DEBUG
    Serial.println("Disconnected");
   #endif
  }

}

//--------------------------------------------------------------------
// Get NTP time, convert from unix time and send via RF
//--------------------------------------------------------------------

#ifdef USE_RFM12B
  void getTime() {
    #ifdef DEBUG  
    Serial.println();
    Serial.println("Getting NTP Time");
    #endif

    sendNTPpacket(timeServer);                                           // Send NTP packet to time server
    delay(1000);                                                         // Wait for reply

    if ( Udp.parsePacket() ) {                                           // Packet received
      timeTX = millis();                                                 // Reset timer
      Udp.read(packetBuffer,NTP_PACKET_SIZE);                            // Read packet into the buffer
      unsigned long highWord = word(packetBuffer[40], packetBuffer[41]); // Timestamp starts at byte 40 & is 4 bytes
      unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);  // or 2 words. Extract the two words.
      unsigned long secsSince1900 = highWord << 16 | lowWord;            // Combine into a long integer

      // Now convert NTP time into everyday time:
      const unsigned long seventyYears = 2208988800UL;                   // Unix time starts on 1/1/1970
      unsigned long epoch = secsSince1900 - seventyYears;                // Subtract seventy years:
      ntp.hour = ((epoch  % 86400L) / 3600);                             // 86400 equals secs per day
      ntp.hour += 1;                                                     // TO DO: Implement something for BST
      ntp.mins = ((epoch  % 3600) / 60);                                 // 3600 equals secs per minute
      ntp.sec = (epoch %60);                                             // seconds

      // Send the time via RF
      int i = 0; while (!rf12_canSend() && i<10) {rf12_recvDone(); i++;}
      rf12_sendStart(0, &ntp, sizeof ntp);                        
      rf12_sendWait(0);
      #ifdef DEBUG  
      Serial.print("Time sent: ");
      Serial.print(ntp.hour);
      Serial.print(":");
      if ( ntp.mins < 10 ) { Serial.print('0'); }
      Serial.print(ntp.mins);
      Serial.print(":");
      if ( ntp.sec < 10 ) { Serial.print('0'); }
      Serial.println(ntp.sec);
      Serial.println();
      #endif
    } else
    {
      #ifdef DEBUG  
      Serial.println("Didn't get reply from NTP server");
      Serial.println();
      #endif
      timeTX = (millis()-30000); // try again in 30 seconds
    }
  }

//--------------------------------------------------------------------
// send an NTP request to the time server at the given address 
//--------------------------------------------------------------------

  unsigned long sendNTPpacket(IPAddress& address) {
    memset(packetBuffer, 0, NTP_PACKET_SIZE); // set all bytes in the buffer to 0

    // Initialize values needed to form NTP request
    packetBuffer[0] = 0b11100011;   // LI, Version, Mode
    packetBuffer[1] = 0;            // Stratum, or type of clock
    packetBuffer[2] = 6;            // Polling Interval
    packetBuffer[3] = 0xEC;         // Peer Clock Precision

    // 8 bytes of zero for Root Delay & Root Dispersion
    packetBuffer[12]  = 49; 
    packetBuffer[13]  = 0x4E;
    packetBuffer[14]  = 49;
    packetBuffer[15]  = 52;

    // Send a packet requesting a timestamp: 		   
    Udp.beginPacket(address, 123); //NTP requests are to port 123
    Udp.write(packetBuffer,NTP_PACKET_SIZE);
    Udp.endPacket(); 
  }
#endif

