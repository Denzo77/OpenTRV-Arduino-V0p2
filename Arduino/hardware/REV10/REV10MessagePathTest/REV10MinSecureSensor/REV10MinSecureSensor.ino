/*
The OpenTRV project licenses this file to you
under the Apache Licence, Version 2.0 (the "Licence");
you may not use this file except in compliance
with the Licence. You may obtain a copy of the Licence at

http://www.apache.org/licenses/LICENSE-2.0 

Unless required by applicable law or agreed to in writing,
software distributed under the Licence is distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied. See the Licence for the
specific language governing permissions and limitations
under the Licence.

Author(s) / Copyright (s): Damon Hart-Davis 2013--2017
                           Deniz Erbilgin 2015--2016
*/

/*
  Test of minimum relay code path.

  DHD20130417: hardware setup on bare board.
    * 1MHz CPU clock (from 8MHz internal RC clock with /8 prescaler) ATmega328P running at 1.8V--5V (typically 2V--3.3V).
    * Fuse set for BOD-managed additional clock settle time, ie as fast a restart from sleep as possible.
    * All unused pins unconnected and nominally floating (though driven low as output where possible).
    * 32768Hz xtal between pins XTAL1 and XTAL2, async timer 2, for accurate timekeeping and low-power sleep.
    * All unused system modules turned off.

  Basic AVR power consumption ticking an (empty) control loop at ~0.5Hz should be ~1uA.
 */

// GLOBAL flags that alter system build and behaviour.
//#define DEBUG // If defined, do extra checks and serial logging.  Will take more code space and power.

// Ensure that OpenTRV 'standard' UART speed is set unless explicitly overridden.
#define BAUD 4800
// Global flag for REV10 secure BHR
#define CONFIG_REV10_SECURE_BHR // REV10: secure stats relay and boiler hub.

// Get defaults for valve applications.
#include <OTV0p2_valve_ENABLE_defaults.h>
// REV8 + GSM Arduino shield + I2CEXT, see TODO-551.
#include <OTV0p2_CONFIG_REV10.h>
// --------------------------------------------
// Fixups to apply after loading the target config.
#include <OTV0p2_valve_ENABLE_fixups.h>

#include <OTV0p2_Board_IO_Config.h> // I/O pin allocation and setup: include ahead of I/O module headers.

#include <Arduino.h>
#include <OTV0p2Base.h>
#include <OTRadioLink.h>
#include <OTRFM23BLink.h>
#include <OTSIM900Link.h>
#include <OTAESGCM.h>

#include "ipAddress.h"  // IP adress in seperate header to avoid accidentally committing.

/////// RADIOS
//For EEPROM: TODO make a spec for how config should be stored in EEPROM to make changing them easy
//- Set the first field of SIM900LinkConfig to true.
//- The configs are stored as \0 terminated strings starting at 0x300.
//- You can program the eeprom using ./OTRadioLink/dev/utils/sim900eepromWrite.ino
//  static const void *SIM900_PIN      = (void *)0x0300;
//  static const void *SIM900_APN      = (void *)0x0305;
//  static const void *SIM900_UDP_ADDR = (void *)0x031B;
//  static const void *SIM900_UDP_PORT = (void *)0x0329;
//  const OTSIM900Link::OTSIM900LinkConfig_t SIM900Config(
//                                                  true,
//                                                  SIM900_PIN,
//                                                  SIM900_APN,
//                                                  SIM900_UDP_ADDR,
//                                                  SIM900_UDP_PORT);
//For Flash:
//- Set the first field of SIM900LinkConfig to false.
//- The configs are stored as \0 terminated strings.
//- Where multiple options are available, uncomment whichever you want
  static const char SIM900_PIN[5] PROGMEM       = "1111";

// APN Configs - Uncomment based on what SIM you are using
//  static const char SIM900_APN[] PROGMEM      = "\"everywhere\",\"eesecure\",\"secure\""; // EE
//static const char SIM900_APN[] PROGMEM      = "\"arkessa.net\",\"arkessa\",\"arkessa\""; // Arkessa
static const char SIM900_APN[] PROGMEM      = "\"mobiledata\""; // GeoSIM

// UDP Configs - Edit SIM900_UDP_ADDR for relevant server. NOTE: The server IP address should never be committed to GitHub.
// IP adress in seperate header to avoid accidentally committing.
//static const char SIM900_UDP_ADDR[16] PROGMEM = ""; // Of form "1.2.3.4". 
static const char SIM900_UDP_PORT[5] PROGMEM = "9999";             // Standard port for OpenTRV servers
const OTSIM900Link::OTSIM900LinkConfig_t SIM900Config(
                                                false,
                                                SIM900_PIN,
                                                SIM900_APN,
                                                SIM900_UDP_ADDR,
                                                SIM900_UDP_PORT);

// Pick an appropriate radio config for RFM23 (if it is the primary radio).
// Nodes talking on fast GFSK channel 0.
static constexpr uint8_t nPrimaryRadioChannels = 1;
static const OTRadioLink::OTRadioChannelConfig RFM23BConfigs[nPrimaryRadioChannels] =
  {
  // GFSK channel 0 full config, RX/TX, not in itself secure.
  OTRadioLink::OTRadioChannelConfig(OTRFM23BLink::StandardRegSettingsGFSK57600, true),
  };

static const OTRadioLink::OTRadioChannelConfig SecondaryRadioConfig(&SIM900Config, true);

// Brings in necessary radio libs.
static constexpr uint8_t RFM23B_RX_QUEUE_SIZE = OTRFM23BLink::DEFAULT_RFM23B_RX_QUEUE_CAPACITY;
static constexpr int8_t RFM23B_IRQ_PIN = PIN_RFM_NIRQ;
static constexpr bool RFM23B_allowRX = true;
OTRFM23BLink::OTRFM23BLink<OTV0P2BASE::V0p2_PIN_SPI_nSS, RFM23B_IRQ_PIN, RFM23B_RX_QUEUE_SIZE, RFM23B_allowRX> RFM23B;
OTSIM900Link::OTSIM900Link<8, 5, RADIO_POWER_PIN, OTV0P2BASE::getSecondsLT> SIM900; // (REGULATOR_POWERUP, RADIO_POWER_PIN);

// Assigns radio to PrimaryRadio alias
OTRadioLink::OTRadioLink &PrimaryRadio = RFM23B;

// Assign radio to SecondaryRadio alias.
OTRadioLink::OTRadioLink &SecondaryRadio = SIM900;

/////// SENSORS

// Sensor for supply (eg battery) voltage in millivolts.
OTV0P2BASE::SupplyVoltageCentiVolts Supply_cV;
OTV0P2BASE::RoomTemperatureC16_TMP112 TemperatureC16;
// Sense ambient lighting level.
OTV0P2BASE::SensorAmbientLight AmbLight;


//========================================
// LOCAL FUNCTIONS
//========================================
// Indicate that the system is broken in an obvious way (distress flashing the main LED).
// DOES NOT RETURN.
// Tries to turn off most stuff safely that will benefit from doing so, but nothing too complex.
// Tries not to use lots of energy so as to keep distress beacon running for a while.
static void panic()
{
    // Reset radio and go into low-power mode.
    PrimaryRadio.panicShutdown();
    // Reset radio and go into low-power mode.
    SecondaryRadio.panicShutdown();
    // Power down almost everything else...
    OTV0P2BASE::minimisePowerWithoutSleep();
    pinMode(OTV0P2BASE::LED_HEATCALL_L, OUTPUT);
    for( ; ; ) {
        OTV0P2BASE::LED_HEATCALL_ON();
        OTV0P2BASE::nap(WDTO_15MS);
        OTV0P2BASE::LED_HEATCALL_OFF();
        OTV0P2BASE::nap(WDTO_120MS);
    }
}
// Panic with fixed message.
static void panic(const __FlashStringHelper *s)
{
    OTV0P2BASE::serialPrintlnAndFlush(); // Start new line to highlight error.  // May fail.
    OTV0P2BASE::serialPrintAndFlush('!'); // Indicate error with leading '!' // May fail.
    OTV0P2BASE::serialPrintlnAndFlush(s); // Print supplied detail text. // May fail.
    panic();
}

// Call this to do an I/O poll if needed; returns true if something useful definitely happened.
// This call should typically take << 1ms at 1MHz CPU.
// Does not change CPU clock speeds, mess with interrupts (other than possible brief blocking), or sleep.
// Should also do nothing that interacts with Serial.
// Limits actual poll rate to something like once every 8ms, unless force is true.
//   * force if true then force full poll on every call (ie do not internally rate-limit)
// Note that radio poll() can be for TX as well as RX activity.
// Not thread-safe, eg not to be called from within an ISR.
// FIXME trying to move into utils (for the time being.)
bool pollIO(const bool force = false)
  {
  static volatile uint8_t _pO_lastPoll;
  // Poll RX at most about every ~8ms.
  const uint8_t sct = OTV0P2BASE::getSubCycleTime();
  if(force || (sct != _pO_lastPoll))
    {
    _pO_lastPoll = sct;
    // Poll for inbound frames.
    // If RX is not interrupt-driven then
    // there will usually be little time to do this
    // before getting an RX overrun or dropped frame.
    PrimaryRadio.poll();
    SecondaryRadio.poll();
    }
  return(false);
  }

#if 0
// Messaging
// Setup frame RX handlers
// Define queue handler
// Currently 4 possible cases for RXing secure frames:
// - Both relay and boiler hub present (e.g. CONFIG_REV10_AS_BHR)
// - Just relay present (e.g. CONFIG_REV10_AS_GSM_RELAY_ONLY)
// - Just boiler hub (e.g. CONFIG_REV8_SECURE_BHR)
// - Unit acting as stats-hub (e.g. CONFIG_REV11_SECURE_STATSHUB)
// relay
inline bool decodeAndHandleSecureFrame(volatile const uint8_t * const msg)
{
  return OTRadioLink::decodeAndHandleOTSecureOFrame<OTRadioLink::SimpleSecureFrame32or0BodyRXV0p2,
                                                    OTAESGCM::fixed32BTextSize12BNonce16BTagSimpleDec_DEFAULT_STATELESS,
                                                   OTV0P2BASE::getPrimaryBuilding16ByteSecretKey,
                                                   OTRadioLink::relayFrameOperation<decltype(SIM900), SIM900>
                                                  >(msg);
}
OTRadioLink::OTMessageQueueHandler< pollIO, V0P2_UART_BAUD,
                                    decodeAndHandleSecureFrame, OTRadioLink::decodeAndHandleDummyFrame
                                   > messageQueue;  //TODO change baud
#endif

//========================================
// INTERRUPT SERVICE ROUTINES
//========================================
// Controller's view of Least Significant Digits of the current (local) time, in this case whole seconds.
// TIME_LSD ranges from 0 to TIME_CYCLE_S-1, also major cycle length.
static constexpr uint_fast8_t TIME_CYCLE_S = 60;
// Controller's notion/cache of seconds within major cycle.
static uint_fast8_t TIME_LSD;

// 'Elapsed minutes' count of minute/major cycles; cheaper than accessing RTC and not tied to real time.
// Starts at or just above zero (within the first 4-minute cycle) to help avoid collisions between units after mass power-up.
// Wraps at its maximum (0xff) value.
static uint8_t minuteCount;

// Mask for Port B input change interrupts.
static constexpr uint8_t RFM23B_INT_MASK = (1 << (PIN_RFM_NIRQ&7));

// Previous state of port B pins to help detect changes.
static volatile uint8_t prevStatePB;
// Interrupt service routine for PB I/O port transition changes.
ISR(PCINT0_vect)
{
    const uint8_t pins = PINB;
    const uint8_t changes = pins ^ prevStatePB;
    prevStatePB = pins;
    // RFM23B nIRQ falling edge is of interest.
    // Handler routine not required/expected to 'clear' this interrupt.
    // TODO: try to ensure that OTRFM23BLink.handleInterruptSimple() is inlineable to minimise ISR prologue/epilogue time and space.
    if((changes & RFM23B_INT_MASK) && !(pins & RFM23B_INT_MASK))
        { PrimaryRadio.handleInterruptSimple(); }
}


// Send the underlying stats binary/text 'whitened' message.
// This must be terminated with an 0xff (which is not sent),
// and no longer than STATS_MSG_MAX_LEN bytes long in total (excluding the terminating 0xff).
// This must not contain any 0xff and should not contain long runs of 0x00 bytes.
// The message to be sent must be written at an offset of STATS_MSG_START_OFFSET from the start of the buffer.
// This routine will alter the content of the buffer for transmission,
// and the buffer should not be re-used as is.
//   * doubleTX  double TX to increase chance of successful reception
//   * RFM23BfriendlyPremable  if true then add an extra preamble
//     to allow RFM23B-based receiver to RX this
// This will use whichever transmission medium/carrier/etc is available.
static constexpr uint8_t RFM22_PREAMBLE_BYTES = 5; // Recommended number of preamble bytes for reliable reception.
static constexpr uint8_t RFM22_SYNC_MIN_BYTES = 3; // Minimum number of sync bytes.
static constexpr uint8_t STATS_MSG_START_OFFSET = (RFM22_PREAMBLE_BYTES + RFM22_SYNC_MIN_BYTES);
static constexpr uint8_t STATS_MSG_MAX_LEN = (64 - STATS_MSG_START_OFFSET);
// Managed JSON stats.
static OTV0P2BASE::SimpleStatsRotation<12> ss1; // Configured for maximum different stats.  // FIXME increased for voice & for setback lockout
// Do bare stats transmission.
// Output should be filtered for items appropriate
// to current channel security and sensitivity level.
// This may be binary or JSON format.
//   * allowDoubleTX  allow double TX to increase chance of successful reception
//   * doBinary  send binary form if supported, else JSON form if supported
// Sends stats on primary radio channel 0 with possible duplicate to secondary channel.
// If sending encrypted then ID/counter fields (eg @ and + for JSON) are omitted
// as assumed supplied by security layer to remote recipent.
static void bareStatsTX()
{
    // Capture heavy stack usage from local allocations here.
    OTV0P2BASE::MemoryChecks::recordIfMinSP();
  
    // Note if radio/comms channel is itself framed.
    const bool neededWaking = OTV0P2BASE::powerUpSerialIfDisabled<>();
  
static_assert(OTV0P2BASE::FullStatsMessageCore_MAX_BYTES_ON_WIRE <= STATS_MSG_MAX_LEN, "FullStatsMessageCore_MAX_BYTES_ON_WIRE too big");
static_assert(OTV0P2BASE::MSG_JSON_MAX_LENGTH+1 <= STATS_MSG_MAX_LEN, "MSG_JSON_MAX_LENGTH too big"); // Allow 1 for trailing CRC.

    // Allow space in buffer for:
    //   * buffer offset/preamble
    //   * max binary length, or max JSON length + 1 for CRC + 1 to allow detection of oversize message
    //   * terminating 0xff
    // Buffer need be no larger than leading length byte + typical 64-byte radio module TX buffer limit + optional terminator.
    const uint8_t MSG_BUF_SIZE = 1 + 64 + 1;
    uint8_t buf[MSG_BUF_SIZE];

    // Send binary *or* JSON on each attempt so as not to overwhelm the receiver.
    {
        // Send JSON message.
        bool sendingJSONFailed = false; // Set true and stop attempting JSON send in case of error.
    
        // Set pointer location based on whether start of message will have preamble TODO move to OTRFM23BLink queueToSend?
        uint8_t *bptr = buf;
        // Leave space for possible leading frame-length byte, eg for encrypted frame.
        ++bptr;
        // Where to write the real frame content.
        uint8_t *const realTXFrameStart = bptr;
    
        // If forcing encryption or if unconditionally suppressed
        // then suppress the "@" ID field entirely,
        // assuming that the encrypted commands will carry the ID, ie in the 'envelope'.
        ss1.setID(V0p2_SENSOR_TAG_F(""));
    
        // Managed JSON stats.
        // Enable "+" count field for diagnostic purposes, eg while TX is lossy,
        // if the primary radio channel does not include a sequence number itself.
        // Assume that an encrypted channel will provide its own (visible) sequence counter.
        ss1.enableCount(false); 
        ss1.putOrRemove(OTV0P2BASE::ErrorReporter);
        ss1.put(TemperatureC16);
        // OPTIONAL items
        constexpr uint8_t privacyLevel = OTV0P2BASE::stTXalwaysAll;
    
        // Buffer to write JSON to before encryption.
        // Size for JSON in 'O' frame is:
        const uint8_t maxSecureJSONSize = OTRadioLink::ENC_BODY_SMALL_FIXED_PTEXT_MAX_SIZE - 2 + 1;
        // writeJSON() requires two further bytes including one for the trailing '\0'.
        uint8_t ptextBuf[maxSecureJSONSize + 2];
    
        // Redirect JSON output appropriately.
        uint8_t *const bufJSON = ptextBuf;
        const uint8_t bufJSONlen = sizeof(ptextBuf);
    
        // Number of bytes written for body.
        // For non-secure, this is the size of the JSON text.
        // For secure this is overridden with the secure frame size.
        int8_t wrote = 0;
    
        // Generate JSON text.
        if(!sendingJSONFailed) {
            // Generate JSON and write to appropriate buffer:
            // direct to TX buffer if not encrypting, else to separate buffer.
            wrote = ss1.writeJSON(bufJSON, bufJSONlen, privacyLevel, true);
            if(0 == wrote) {
                sendingJSONFailed = true;
            }
        }

        // Push the JSON output to Serial.
        if(!sendingJSONFailed) {
            // Insert synthetic full ID/@ field for local stats, but no sequence number for now.
            Serial.print(F("{\"@\":\""));
            for(int i = 0; i < OTV0P2BASE::OpenTRV_Node_ID_Bytes; ++i) { Serial.print(eeprom_read_byte((uint8_t *)V0P2BASE_EE_START_ID+i), HEX); }
            Serial.print(F("\","));
            Serial.write(bufJSON+1, wrote-1);
            Serial.println();
            OTV0P2BASE::flushSerialSCTSensitive(); // Ensure all flushed since system clock may be messed with...
        } 
    
        // Get the 'building' key for stats sending.
        uint8_t key[16];
        if(!sendingJSONFailed) {
            if(!OTV0P2BASE::getPrimaryBuilding16ByteSecretKey(key)) {
                sendingJSONFailed = true;
                OTV0P2BASE::serialPrintlnAndFlush(F("!TX key")); // Know why TX failed.
            }
        }
    
        // If doing encryption
        // then build encrypted frame from raw JSON.
        if(!sendingJSONFailed) {
            // Explicit-workspace version of encryption.
            const OTRadioLink::SimpleSecureFrame32or0BodyTXBase::fixed32BTextSize12BNonce16BTagSimpleEncWithLWorkspace_ptr_t eW = OTAESGCM::fixed32BTextSize12BNonce16BTagSimpleEnc_DEFAULT_WITH_LWORKSPACE;
            constexpr size_t workspaceSize = OTRadioLink::SimpleSecureFrame32or0BodyTXBase::generateSecureOFrameRawForTX_total_scratch_usage_OTAESGCM_2p0;
            uint8_t workspace[workspaceSize];
            OTV0P2BASE::ScratchSpaceL sW(workspace, workspaceSize);
            const uint8_t txIDLen = OTRadioLink::ENC_BODY_DEFAULT_ID_BYTES;
            // When sending on a channel with framing, do not explicitly send the frame length byte.
            constexpr uint8_t offset = 1;
            
            // Assumed to be at least one free writeable byte ahead of bptr.
            // Distinguished 'invalid' valve position; never mistaken for a real valve.
            const uint8_t valvePC = 0x7f;
            const uint8_t bodylen = OTRadioLink::SimpleSecureFrame32or0BodyTXV0p2::getInstance().generateSecureOFrameRawForTX(
                  realTXFrameStart - offset, sizeof(buf) - (realTXFrameStart-buf) + offset,
                  txIDLen, valvePC, (const char *)bufJSON, eW, sW, key);
            sendingJSONFailed = (0 == bodylen);
            if (sendingJSONFailed) OTV0P2BASE::serialPrintlnAndFlush(F("!TX Enc")); // Know why TX failed.
            wrote = bodylen - offset;
        }
    
        if(!sendingJSONFailed) {
            // Write out unadjusted JSON or encrypted frame on secondary radio.
            // Assumes that framing (or not) of primary and secondary radios is the same (usually: both framed).
            SecondaryRadio.queueToSend(realTXFrameStart, wrote);
        }
        pollIO(); // Serial must already be running!
        if(!sendingJSONFailed) {
            // Send directly to the primary radio...
            if(!PrimaryRadio.queueToSend(realTXFrameStart, wrote)) { sendingJSONFailed = true; }
        }
    }
    if(neededWaking) { OTV0P2BASE::flushSerialProductive(); OTV0P2BASE::powerDownSerial(); }
}

//========================================
// SETUP
//========================================

// Setup routine: runs once after reset.
// Does some limited board self-test and will panic() if anything is obviously broken.
static constexpr uint8_t PP_OFF_MS = 250;
void setup()
{
    //----------- Low level setup
    // Set appropriate low-power states, interrupts, etc, ASAP.
    OTV0P2BASE::powerSetup();
  
    // IO setup for safety, and to avoid pins floating.
    OTV0P2BASE::IOSetup();
    
    OTV0P2BASE::serialPrintAndFlush(F("\r\nOpenTRV: ")); // Leading CRLF to clear leading junk, eg from bootloader.
      V0p2Base_serialPrintlnBuildVersion();
    
    // Count resets to detect unexpected crashes/restarts.
    const uint8_t oldResetCount = eeprom_read_byte((uint8_t *)V0P2BASE_EE_START_RESET_COUNT);
    eeprom_write_byte((uint8_t *)V0P2BASE_EE_START_RESET_COUNT, 1 + oldResetCount);
    
    // Have 32678Hz clock at least running before going any further.
    // Check that the slow clock is running reasonably OK, and tune the fast one to it.
    if(!::OTV0P2BASE::HWTEST::calibrateInternalOscWithExtOsc()) { panic(F("Xtal")); } // Async clock not running or can't tune.
  
    //----------- Flash UI
    // Signal that xtal is running AND give it time to settle.
    OTV0P2BASE::sleepLowPowerMs(1000);
    OTV0P2BASE::LED_HEATCALL_OFF();
  
    OTV0P2BASE::sleepLowPowerMs(PP_OFF_MS); // TODO: use this time to gather entropy.
    OTV0P2BASE::LED_HEATCALL_ON();
    OTV0P2BASE::sleepLowPowerMs(1000); // TODO: use this time to gather entropy.
  
  
    //----------- Init Radio
    // Initialise the radio, if configured, ASAP because it can suck a lot of power until properly initialised.
    PrimaryRadio.preinit(NULL);
    // Check that the radio is correctly connected; panic if not...
    if(!PrimaryRadio.configure(nPrimaryRadioChannels, RFM23BConfigs) || !PrimaryRadio.begin()) { panic(F("r1")); }
  
    // Turn power on for SIM900 with PFET for secondary power control.
    fastDigitalWrite(A3, 0); // todo move into sim900link
    pinMode(A3, OUTPUT);
    // Initialise the radio, if configured, ASAP because it can suck a lot of power until properly initialised.
    SecondaryRadio.preinit(NULL);
    // Check that the radio is correctly connected; panic if not...
    if(!SecondaryRadio.configure(1, &SecondaryRadioConfig) || !SecondaryRadio.begin()) { panic(F("r2")); }
  
    //----------- Init sensors
    // Collect full set of environmental values before entering loop() in normal mode.
    // This should also help ensure that sensors are properly initialised.
    TemperatureC16.read();
    Supply_cV.read();
  
    OTV0P2BASE::seedPRNGs();
  
    //----------- Ensure has ID
    // Ensure that the unique node ID is set up (mainly on first use).
    // Have one attempt (don't want to stress an already failing EEPROM) to force-reset if not good, then panic.
    // Needs to have had entropy gathered, etc.
    if(!OTV0P2BASE::ensureIDCreated())
      {
      if(!OTV0P2BASE::ensureIDCreated(true)) // Force reset.
        { panic(F("ID")); }
      }
  
    // Initialised: turn main/heatcall UI LED off.
    OTV0P2BASE::LED_HEATCALL_OFF();
  
    // Do OpenTRV-specific (late) setup.
      // Radio not listening to start with.
    // Ignore any initial spurious RX interrupts for example.
    PrimaryRadio.listen(false);
  
    // Set up async edge interrupts.
    ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
    { 
        PCICR = 1 ;//| 4;  // 0x1 enables PB/PCMSK0. 0x4 enables PD/PCMSK2.
        PCMSK0 = RFM23B_INT_MASK; 
    }
  
    // Do early 'wake-up' stats transmission if possible
    // when everything else is set up and ready and allowed (TODO-636)
    // including all set-up and inter-wiring of sensors/actuators.
    // Attempt to maximise chance of reception with a double TX.
    // Assume not in hub mode (yet).
    // Send all possible formats, binary first (assumed complete in one message).
    bareStatsTX();
    // Send JSON stats repeatedly (typically once or twice)
    // until all values pushed out (no 'changed' values unsent)
    // or limit reached.
    for(uint8_t i = 5; --i > 0; ) {
        ::OTV0P2BASE::nap(WDTO_120MS, false); // Sleep long enough for receiver to have a chance to process previous TX.
        bareStatsTX();
        if(!ss1.changedValue()) { break; }
    }
    
    // Start local counters in randomised positions to help avoid inter-unit collisions,
    // eg for mains-powered units starting up together after a power cut,
    // but without (eg) breaking any of the logic about what order things will be run first time through.
    // Uses some decent noise to try to start the units separated.
    const uint8_t b = OTV0P2BASE::getSecureRandomByte(); // randRNG8();
    // Start within bottom half of minute (or close to); sensor readings happen in second half.
    OTV0P2BASE::setSeconds(b >> 2);
    // Start anywhere in first 4 minute cycle.
    minuteCount = b & 3;
    // Set appropriate loop() values just before entering it.
    TIME_LSD = OTV0P2BASE::getSecondsLT();
}

//========================================
// MAIN LOOP
//========================================

void loop()
{
    // Force restart if SPAM/heap/stack likely corrupt.
    OTV0P2BASE::MemoryChecks::forceResetIfStackOverflow();
    // Complain and keep complaining when getting near stack overflow.
    // TODO: make DEBUG-only when confident all configs OK.
    const int16_t minsp = OTV0P2BASE::MemoryChecks::getMinSPSpaceBelowStackToEnd();
    if(minsp < 64) { OTV0P2BASE::serialPrintlnAndFlush(F("!SH")); }
  
        // Sensor readings are taken late in each minute (where they are taken)
    // and if possible noise and heat and light should be minimised in this part of each minute to improve readings.
    // Sensor readings and (stats transmissions) are nominally on a 4-minute cycle.
    const uint8_t minuteFrom4 = (minuteCount & 3);
    // The 0th minute in each group of four is always used for measuring where possible (possibly amongst others)
    // and where possible locally-generated noise and heat and light should be minimised in this minute
    // to give the best possible readings.
    // True if this is the first (0th) minute in each group of four.
    const bool minute0From4ForSensors = (0 == minuteFrom4);
    // True if this is the minute after all sensors should have been sampled.
    const bool minute1From4AfterSensors = (1 == minuteFrom4);
  
    // Sleep in low-power mode (waiting for interrupts) until seconds roll.
    // NOTE: sleep at the top of the loop to minimise timing jitter/delay from Arduino background activity after loop() returns.
    // DHD20130425: waking up from sleep and getting to start processing below this block may take >10ms.
    // Ensure that serial I/O is off while sleeping.
    OTV0P2BASE::powerDownSerial();
    // Power down most stuff (except radio for hub RX).
    OTV0P2BASE::minimisePowerWithoutSleep();
    uint_fast8_t newTLSD;
    while(TIME_LSD == (newTLSD = OTV0P2BASE::getSecondsLT())) {
        // Poll I/O and process message incrementally (in this otherwise idle time)
        // before sleep and on wakeup in case some IO needs further processing now,
        // eg work was accrued during the previous major slow/outer loop
        // or the in a previous orbit of this loop sleep or nap was terminated by an I/O interrupt.
        // May generate output to host on Serial.
        // Come back and have another go immediately until no work remaining.
        pollIO();
        // Normal long minimal-power sleep until wake-up interrupt.
        // Rely on interrupt to force quick loop round to I/O poll.
        OTV0P2BASE::sleepUntilInt();
    }
    TIME_LSD = newTLSD;
    // Reset and immediately re-prime the RTC-based watchdog.
    OTV0P2BASE::resetRTCWatchDog();
    OTV0P2BASE::enableRTCWatchdog(true);
  
  
    // START LOOP BODY
    // ===============
  
    // High-priority UI handing, every other/even second.
    // Show status if the user changed something significant.
    // Must take ~300ms or less so as not to run over into next half second if two TXs are done.
  
    // Handling the UI may have taken a little while, so process I/O a little.
    pollIO(); // Deal with any pending I/O.
    
    // DO SCHEDULING
    
    // Run some tasks less often when not demanding heat (at the valve or boiler), so as to conserve/energy.
    // Spare the batteries if they are low, or the unit is in FROST mode, or if the room/area appears to be vacant.
    // Stay responsive if the valve is open and/or we are otherwise calling for heat.
    // Once-per-minute tasks: all must take << 0.3s unless particular care is taken.
    // Run tasks spread throughout the minute to be as kind to batteries (etc) as possible.
    // Only when runAll is true run less-critical tasks that be skipped sometimes when particularly conserving energy.
    // Run all for first full 4-minute cycle, eg because unit may start anywhere in it.
    // Note: ensure only take ambient light reading at times when all LEDs are off (or turn them off).
    // TODO: coordinate temperature reading with time when radio and other heat-generating items are off for more accurate readings.
    const bool runAll = minute0From4ForSensors || (minuteCount < 4);
  
    switch(TIME_LSD) // With V0P2BASE_TWO_S_TICK_RTC_SUPPORT only even seconds are available.
    {
    case 0:
        {
            // Tasks that must be run every minute.
            ++minuteCount; // Note simple roll-over to 0 at max value.
            break;
        }
  
    // Churn/reseed PRNG(s) a little to improve unpredictability in use: should be lightweight.
    case 2: { if(runAll) { OTV0P2BASE::seedRNG8(minuteCount ^ OTV0P2BASE::getCPUCycleCount() ^ (uint8_t)Supply_cV.get(), OTV0P2BASE::_getSubCycleTime() ^ AmbLight.get(), (uint8_t)TemperatureC16.get()); } break; }
    // Force read of supply/battery voltage; measure and recompute status (etc) less often when already thought to be low, eg when conserving.
    case 4: { if(runAll) { Supply_cV.read(); } break; }

    // Periodic transmission of stats if NOT driving a local valve (else stats can be piggybacked onto that).
    // Randomised somewhat between slots and also within the slot to help avoid collisions.
    static uint8_t txTick;
    case 6: { txTick = OTV0P2BASE::randRNG8() & 7; break; } // Pick which of the 8 slots to use.
    case 8: case 10: case 12: case 14: case 16: case 18: case 20: case 22:
        {
            // Only the slot where txTick is zero is used.
            if(0 != txTick--) { break; }
      
            // Stats TX in the minute (#1) after all sensors should have been polled
            // (so that readings are fresh) and evenly between.
            // Usually send one frame every 4 minutes, 2 if this is a valve.
            // No extra stats TX for changed data to reduce information/activity leakage.
            // Note that all O frames contain the current valve percentage,
            // which implies that any extra stats TX also speeds response to call-for-heat changes.
            if(!minute1From4AfterSensors) { break; }
      
            // Sleep randomly up to ~25% of the minor cycle
            // to spread transmissions and thus help avoid collisions.
            // (Longer than 25%/0.5s could interfere with other ops such as FHT8V TXes.)
            const uint8_t stopBy = 1 + (((OTV0P2BASE::GSCT_MAX >> 2) | 7) & OTV0P2BASE::randRNG8());
            while(OTV0P2BASE::getSubCycleTime() <= stopBy) {
                // Handle any pending I/O while waiting.
                pollIO();
                // Sleep a little.
                OTV0P2BASE::nap(WDTO_15MS, true);
              }
      
            // Send stats!
            // Try for double TX for extra robustness unless:
            //   * this is a speculative 'extra' TX
            //   * battery is low
            //   * this node is a hub so needs to listen as much as possible
            // This doesn't generally/always need to send binary/both formats
            // if this is controlling a local FHT8V on which the binary stats can be piggybacked.
            // Ie, if doesn't have a local TRV then it must send binary some of the time.
            // Any recently-changed stats value is a hint that a strong transmission might be a good idea.
            bareStatsTX();
            break;
        }
  
  // SENSOR READ AND STATS
  //
  // All external sensor reads should be in the second half of the minute (>32) if possible.
  // This is to have them as close to stats collection at the end of the minute as possible,
  // and to allow randomisation of the start-up cycle position in the first 32s to help avoid inter-unit collisions.
  // Also all sources of noise, self-heating, etc, may be turned off for the 'sensor read minute'
  // and thus will have diminished by this point.
  
      // At a hub, sample temperature regularly as late as possible in the minute just before recomputing valve position.
      // Force a regular read to make stats such as rate-of-change simple and to minimise lag.
      // TODO: optimise to reduce power consumption when not calling for heat.
      // TODO: optimise to reduce self-heating jitter when in hub/listen/RX mode.
      case 54: { TemperatureC16.read(); break; }
  
      // Compute targets and heat demand based on environmental inputs and occupancy.
      // This should happen as soon after the latest readings as possible (temperature especially).
      case 56:
          {
              // Age errors/warnings.
              OTV0P2BASE::ErrorReporter.read();
              break;
          }
    }
  
    // End-of-loop processing, that may be slow.
    // Ensure progress on queued messages ahead of slow work.  (TODO-867)
    pollIO();; // Deal with any pending I/O.
}
