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

Author(s) / Copyright (s): Damon Hart-Davis 2014
*/

/*
 V0p2 boards physical sensor support.
 */
#include <stdint.h>
#include <limits.h>
#include <util/atomic.h>

#include <Wire.h> // Arduino I2C library.

#include "Sensor.h"

#include "V0p2_Main.h"
#include "V0p2_Board_IO_Config.h" // I/O pin allocation: include ahead of I/O module headers.
#include "V0p2_Sensors.h" // I/O code access.

#include "Control.h"
#include "Serial_IO.h"
#include "Power_Management.h"
#include "UI_Minimal.h"






















#ifndef OMIT_MODULE_LDROCCUPANCYDETECTION

// Note on: phototransistor variant.
// Note that if AMBIENT_LIGHT_SENSOR_PHOTOTRANS_TEPT4400 is defined
// This expects a current-response phototransistor in place of the LDR
// with roughly full-scale value in full light against internal 1.1V reference
// not against supply voltage as usual.

#ifdef AMBIENT_LIGHT_SENSOR_PHOTOTRANS_TEPT4400
#define ALREFERENCE INTERNAL // Internal 1.1V reference.
// If defined, then allow adaptive compression of top part of range when would otherwise max out.
// This may be somewhat supply-voltage dependent, eg capped by the supply voltage.
// Supply voltage is expected to be 2--3 times the bandgap reference, typically.
#define ADAPTIVE_THRESHOLD 896U // (1024-128) Top ~10%, companding by 8x.

// This implementation expects a phototransitor TEPT4400 (50nA dark current, nominal 200uA@100lx@Vce=50V) from IO_POWER_UP to LDR_SENSOR_AIN and 220k to ground.
// Measurement should be taken wrt to internal fixed 1.1V bandgap reference, since light indication is current flow across a fixed resistor.
// Aiming for maximum reading at or above 100--300lx, ie decent domestic internal lighting.
// Note that phototransistor is likely far more directionally-sensitive than LDR and its response nearly linear.
// This extends the dynamic range and switches to measurement vs supply when full-scale against bandgap ref, then scales by Vss/Vbandgap and compresses to fit.
// http://home.wlv.ac.uk/~in6840/Lightinglevels.htm
// http://www.engineeringtoolbox.com/light-level-rooms-d_708.html
// http://www.pocklington-trust.org.uk/Resources/Thomas%20Pocklington/Documents/PDF/Research%20Publications/GPG5.pdf
// http://www.vishay.com/docs/84154/appnotesensors.pdf

#if 7 == V0p2_REV // REV7 board uses slightly different phototransistor to TEPT4400.
static const int LDR_THR_LOW = 180U;
static const int LDR_THR_HIGH = 250U;
#else // REV4 default values.
static const int LDR_THR_LOW = 270U;
static const int LDR_THR_HIGH = 400U;
#endif

#else // LDR

// This implementation expects an LDR (1M dark resistance) from IO_POWER_UP to LDR_SENSOR_AIN and 100k to ground.
// Measurement should be taken wrt to supply voltage, since light indication is a fraction of that.
// Values below from PICAXE V0.09 impl approx multiplied by 4+ to allow for scale change.
#define ALREFERENCE DEFAULT // Supply voltage as reference.

#ifdef LDR_EXTRA_SENSITIVE // Define if LDR not exposed to much light, eg for REV2 cut4 sideways-pointing LDR (TODO-209).
static const int LDR_THR_LOW = 50U;
static const int LDR_THR_HIGH = 70U; 
#else // Normal settings.
static const int LDR_THR_LOW = 160U; // Was 30.
static const int LDR_THR_HIGH = 200U; // Was 35.
#endif

#endif

// Measure/store/return the current room ambient light levels in range [0,1023].
// This may consume significant power and time.
// Probably no need to do this more than (say) once per minute.
// This implementation expects LDR (1M dark resistance) from IO_POWER_UP to LDR_SENSOR_AIN and 100k to ground.
// (Not intended to be called from ISR.)
uint8_t AmbientLight::read()
  {
  power_intermittent_peripherals_enable(false); // No need to wait for anything to stablise as direct of IO_POWER_UP.
  const uint16_t al0 = analogueNoiseReducedRead(LDR_SENSOR_AIN, ALREFERENCE);
#if defined(ADAPTIVE_THRESHOLD)
  uint16_t al;
  if(al0 >= ADAPTIVE_THRESHOLD)
    {
    const uint16_t al1 = analogueNoiseReducedRead(LDR_SENSOR_AIN, DEFAULT); // Vsupply reference.
    Supply_mV.read();
    const uint16_t vbg = Supply_mV.getRawInv(); // Vbandgap wrt Vsupply.
    // Compute value in extended range up to ~1024 * Vsupply/Vbandgap.
    const uint16_t ale = ((al1 << 5) / ((vbg+16) >> 5)); // Faster int-only approximation to (int)((al1 * 1024L) / vbg)).
    // Assuming typical V supply of 2--3 times Vbandgap,
    // compress above threshold to extend top of range by a factor of two.
    // Ensure that scale stays monotonic in face of calculation lumpiness, etc...
    // Scale all remaining space above threshold to new top value into remaining space.
    // TODO: ensure scaleFactor is a power of two for speed.
    const uint16_t scaleFactor = (2048 - ADAPTIVE_THRESHOLD) / (1024 - ADAPTIVE_THRESHOLD);
    al = fnmin(1023U,
        ADAPTIVE_THRESHOLD + fnmax(0U, ((ale - ADAPTIVE_THRESHOLD) / scaleFactor)));
#if 0 && defined(DEBUG)
    DEBUG_SERIAL_PRINT_FLASHSTRING("Ambient raw: ");
    DEBUG_SERIAL_PRINT(al0);
    DEBUG_SERIAL_PRINT_FLASHSTRING(", against Vcc: ");
    DEBUG_SERIAL_PRINT(al1);
    DEBUG_SERIAL_PRINT_FLASHSTRING(", Vref against Vcc: ");
    DEBUG_SERIAL_PRINT(vbg);
    DEBUG_SERIAL_PRINT_FLASHSTRING(", extended scale value: ");
    DEBUG_SERIAL_PRINT(ale);
    DEBUG_SERIAL_PRINT_FLASHSTRING(", compressed value: ");
    DEBUG_SERIAL_PRINT(al);
    DEBUG_SERIAL_PRINTLN();
#endif
    }
  else { al = al0; }
#else
  const uint16_t al = al0;
#endif
  power_intermittent_peripherals_disable();

  // Capture entropy from changed LS bits.
  if((uint8_t)al != (uint8_t)rawValue) { addEntropyToPool((uint8_t)al ^ (uint8_t)rawValue, 0); } // Claim zero entropy as may be forced by Eve.

  // Adjust room-lit flag, with hysteresis.
  if(al <= LDR_THR_LOW)
    {
    isRoomLitFlag = false;
    // If dark enough to isRoomLitFlag false then increment counter.
    if(darkTicks < 255) { ++darkTicks; }
    }
  else if(al > LDR_THR_HIGH)
    {
    // Treat a sharp transition from dark to light as a possible/weak indication of occupancy, eg light flicked on.
    // Ignore trigger at start-up.
    static bool ignoreFirst;
    if(!ignoreFirst) { ignoreFirst = true; }
    else if((!isRoomLitFlag) && (rawValue < LDR_THR_LOW)) { Occupancy.markAsPossiblyOccupied(); }
    isRoomLitFlag = true;
    // If light enough to isRoomLitFlag true then reset counter.
    darkTicks = 0;
    }

  // Store new value, raw and normalised.
  // Unconditionbally store raw value.
  rawValue = al;
  // Apply a little bit of noise reduction (hysteresis) to the normalised version.
  const uint8_t newValue = (uint8_t)(al >> 2);
  if(newValue != value)
    {
    const uint16_t oldRawImplied = ((uint16_t)value) << 2;
    const uint16_t absDiff = (oldRawImplied > al) ? (oldRawImplied - al) : (al - oldRawImplied);
    if(absDiff > 2) { value = newValue; }
    }

#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("Ambient light: ");
  DEBUG_SERIAL_PRINT(al);
  DEBUG_SERIAL_PRINTLN();
#endif

#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("isRoomLit: ");
  DEBUG_SERIAL_PRINT(isRoomLitFlag);
  DEBUG_SERIAL_PRINTLN();
#endif

  return(value);
  }

// Singleton implementation/instance.
AmbientLight AmbLight;
#endif











// TMP102 and TMP112 should be interchangeable: latter has better guaranteed accuracy.
#define TMP102_I2C_ADDR 72
#define TMP102_REG_TEMP 0 // Temperature register.
#define TMP102_REG_CTRL 1 // Control register.
#define TMP102_CTRL_B1 0x31 // Byte 1 for control register: 12-bit resolution and shutdown mode (SD).
#define TMP102_CTRL_B1_OS 0x80 // Control register: one-shot flag in byte 1.
#define TMP102_CTRL_B2 0x0 // Byte 2 for control register: 0.25Hz conversion rate and not extended mode (EM).

//// Last temperature read with readTemperatureC16(); initially 0 and set to 0 on error.
//static int temp16;

#ifndef SENSOR_SHT21_ENABLE // Don't use TMP112 if SHT21 is pn the board instead.
// Measure/store/return the current room ambient temperature in units of 1/16th C.
// This may contain up to 4 bits of information to the right of the fixed binary point.
// This may consume significant power and time.
// Probably no need to do this more than (say) once per minute.
// The first read will initialise the device as necessary and leave it in a low-power mode afterwards.
// This will simulate a zero temperature in case of detected error talking to the sensor as fail-safe for this use.
// Check for errors at certain critical places, not everywhere.
static int TMP112_readTemperatureC16()
  {
  const bool neededPowerUp = powerUpTWIIfDisabled();
  
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("TMP102 needed power-up: ");
  DEBUG_SERIAL_PRINT(neededPowerUp);
  DEBUG_SERIAL_PRINTLN();
#endif

  // Force start of new one-shot temperature measurement/conversion to complete.
  Wire.beginTransmission(TMP102_I2C_ADDR);
  Wire.write((byte) TMP102_REG_CTRL); // Select control register.
  Wire.write((byte) TMP102_CTRL_B1); // Clear OS bit.
  //Wire.write((byte) TMP102_CTRL_B2);
  Wire.endTransmission();
  Wire.beginTransmission(TMP102_I2C_ADDR);
  Wire.write((byte) TMP102_REG_CTRL); // Select control register.
  Wire.write((byte) TMP102_CTRL_B1 | TMP102_CTRL_B1_OS); // Start one-shot conversion.
  //Wire.write((byte) TMP102_CTRL_B2);
  if(Wire.endTransmission()) { return(0); } // Exit if error.


  // Wait for temperature measurement/conversion to complete, in low-power sleep mode for the bulk of the time.
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("TMP102 waiting for conversion...");
#endif
  Wire.beginTransmission(TMP102_I2C_ADDR);
  Wire.write((byte) TMP102_REG_CTRL); // Select control register.
  if(Wire.endTransmission()) { return(0); } // Exit if error.
  for(int i = 8; --i; ) // 2 orbits should generally be plenty.
    {
    if(i <= 0) { return(0); } // Exit if error.
    if(Wire.requestFrom(TMP102_I2C_ADDR, 1) != 1) { return(0); } // Exit if error.
    const byte b1 = Wire.read();
    if(b1 & TMP102_CTRL_B1_OS) { break; } // Conversion completed.
    nap(WDTO_15MS); // One or two of these naps should allow typical ~26ms conversion to complete...
    }

  // Fetch temperature.
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("TMP102 fetching temperature...");
#endif
  Wire.beginTransmission(TMP102_I2C_ADDR);
  Wire.write((byte) TMP102_REG_TEMP); // Select temperature register (set ptr to 0).
  if(Wire.endTransmission()) { return(0); } // Exit if error.
  if(Wire.requestFrom(TMP102_I2C_ADDR, 2) != 2)  { return(0); }
  if(Wire.endTransmission()) { return(0); } // Exit if error.

  const byte b1 = Wire.read(); // MSByte, should be signed whole degrees C.
  const uint8_t b2 = Wire.read(); // Avoid sign extension...

  // Builds 12-bit value (assumes not in extended mode) and sign-extends if necessary for sub-zero temps.
  const int t16 = (b1 << 4) | (b2 >> 4) | ((b1 & 0x80) ? 0xf000 : 0);

//  // Store the result for access at any time.
//  temp16 = t16;

#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("TMP102 temp: ");
  DEBUG_SERIAL_PRINT(b1);
  DEBUG_SERIAL_PRINT_FLASHSTRING("C / ");
  DEBUG_SERIAL_PRINT(temp16);
  DEBUG_SERIAL_PRINTLN();
#endif

  if(neededPowerUp) { powerDownTWI(); }

  return(t16);
  }
#endif

//// Return previously-read (with readTemperatureC16()) temperature; very fast.
//int getTemperatureC16() { return(temp16); }







// Functionality and code only enabled if SENSOR_SHT21_ENABLE is defined.
#ifdef SENSOR_SHT21_ENABLE

#define SHT21_I2C_ADDR 0x40
#define SHT21_I2C_CMD_TEMP_HOLD	0xe3
#define SHT21_I2C_CMD_TEMP_NOHOLD 0xf3
#define SHT21_I2C_CMD_RH_HOLD	0xe5
#define SHT21_I2C_CMD_RH_NOHOLD 0xf5
#define SHT21_I2C_CMD_USERREG 0xe7 // User register...

// If defined, sample 8-bit RH (for for 1%) and 12-bit temp (for 1/16C).
// This should save time and energy.
#define SHT21_USE_REDUCED_PRECISION 1

// Set true once SHT21 has been initialised.
static volatile bool SHT21_initialised;

// Initialise/configure SHT21, once only generally.
// TWI must already be powered up.
static void SHT21_init()
  {
#if defined(SHT21_USE_REDUCED_PRECISION)
  // Soft reset in order to sample at reduced precision.
  Wire.beginTransmission(SHT21_I2C_ADDR);
  Wire.write((byte) SHT21_I2C_CMD_USERREG); // Select control register.
  Wire.endTransmission();
  Wire.requestFrom(SHT21_I2C_ADDR, 1);
  while(Wire.available() < 1)
    {
    // Wait for data, but avoid rolling over the end of a minor cycle...
    if(getSubCycleTime() >= GSCT_MAX-2)
      {
      return; // Failed, and not initialised.
      }
    }
  const uint8_t curUR = Wire.read();
//  DEBUG_SERIAL_PRINT_FLASHSTRING("UR: ");
//  DEBUG_SERIAL_PRINTFMT(curUR, HEX);
//  DEBUG_SERIAL_PRINTLN();

  // Preserve reserved bits (3, 4, 5) and sample 8-bit RH (for for 1%) and 12-bit temp (for 1/16C).
  const uint8_t newUR = (curUR & 0x38) | 3;
  Wire.beginTransmission(SHT21_I2C_ADDR);
  Wire.write((byte) SHT21_I2C_CMD_USERREG); // Select control register.
  Wire.write((byte) newUR);
  Wire.endTransmission();

#endif
  SHT21_initialised = true;
  }

// Measure and return the current ambient temperature in units of 1/16th C.
// This may contain up to 4 bits of information to the right of the fixed binary point.
// This may consume significant power and time.
// Probably no need to do this more than (say) once per minute.
// The first read will initialise the device as necessary and leave it in a low-power mode afterwards.
static int Sensor_SHT21_readTemperatureC16()
  {
  const bool neededPowerUp = powerUpTWIIfDisabled();

  // Initialise/config if necessary.
  if(!SHT21_initialised) { SHT21_init(); }

  // Max RH measurement time:
  //   * 14-bit: 85ms
  //   * 12-bit: 22ms
  //   * 11-bit: 11ms
  // Use blocking data fetch for now.
  Wire.beginTransmission(SHT21_I2C_ADDR);
  Wire.write((byte) SHT21_I2C_CMD_TEMP_HOLD); // Select control register.
#if defined(SHT21_USE_REDUCED_PRECISION)
  nap(WDTO_30MS); // Should cover 12-bit conversion (22ms).
#else
  sleepLowPowerMs(90); // Should be plenty for slowest (14-bit) conversion (85ms).
#endif
  //delay(100);
  Wire.endTransmission();
  Wire.requestFrom(SHT21_I2C_ADDR, 3);
  while(Wire.available() < 3)
    {
    // Wait for data, but avoid rolling over the end of a minor cycle...
    if(getSubCycleTime() >= GSCT_MAX-2)
      {
      return(0); // Failure value: may be able to to better.
      }
    }
  uint16_t rawTemp = (Wire.read() << 8);
  rawTemp |= (Wire.read() & 0xfc); // Clear status ls bits.

  // Power down TWI ASAP.
  if(neededPowerUp) { powerDownTWI(); }

  // TODO: capture entropy if (transformed) value has changed.

  // Nominal formula: C = -46.85 + ((175.72*raw) / (1L << 16));
  const int c16 = -750 + ((5623L * rawTemp) >> 17); // FIXME: find a faster approximation...

  return(c16);
  }

// Measure and return the current relative humidity in %; range [0,100] and 255 for error.
// This may consume significant power and time.
// Probably no need to do this more than (say) once per minute.
// The first read will initialise the device as necessary and leave it in a low-power mode afterwards.
// Returns 255 (~0) in case of error.
uint8_t HumiditySensorSHT21::read()
  {
  const bool neededPowerUp = powerUpTWIIfDisabled();

  // Initialise/config if necessary.
  if(!SHT21_initialised) { SHT21_init(); }

  // Get RH%...
  // Max RH measurement time:
  //   * 12-bit: 29ms
  //   *  8-bit:  4ms
  // Use blocking data fetch for now.
  Wire.beginTransmission(SHT21_I2C_ADDR);
  Wire.write((byte) SHT21_I2C_CMD_RH_HOLD); // Select control register.
#if defined(SHT21_USE_REDUCED_PRECISION)
  sleepLowPowerMs(5); // Should cover 8-bit conversion (4ms).
#else
  nap(WDTO_30MS); // Should cover even 12-bit conversion (29ms).
#endif
  Wire.endTransmission();
  Wire.requestFrom(SHT21_I2C_ADDR, 3);
  while(Wire.available() < 3)
    {
    // Wait for data, but avoid rolling over the end of a minor cycle...
    if(getSubCycleTime() >= GSCT_MAX)
      {
//      DEBUG_SERIAL_PRINTLN_FLASHSTRING("giving up");
      return(~0);
      }
    }
  const uint8_t rawRH = Wire.read();
  const uint8_t rawRL = Wire.read();

  // Power down TWI ASAP.
  if(neededPowerUp) { powerDownTWI(); }

  const uint16_t raw = (((uint16_t)rawRH) << 8) | (rawRL & 0xfc); // Clear status ls bits.
  const uint8_t result = -6 + ((125L * raw) >> 16);

  // Capture entropy from raw status bits
  // iff (transformed) reading has changed.
  if(value != result) { addEntropyToPool(rawRL ^ rawRH, 1); }

  value = result;
  if(result > (HUMIDTY_HIGH_RHPC+HUMIDITY_EPSILON_RHPC)) { highWithHyst = true; }
  else if(result < (HUMIDTY_HIGH_RHPC-HUMIDITY_EPSILON_RHPC)) { highWithHyst = false; }
  return(result);
  }
// Singleton implementation/instance.
HumiditySensorSHT21 RelHumidity;
#endif


//// Median filter.
//// Find mean of interquatile range of group of ints where sum can be computed in an int without loss.
//// FIXME: needs a unit test or three.
//template<uint8_t N> int smallIntIQMean(const int data[N])
//  {
//  // Copy array content.
//  int copy[N];
//  for(int8_t i = N; --i >= 0; ) { copy[i] = data[i]; }
//  // Sort in place with a bubble sort (yeuck) assuming the array to be small.
//  // FIXME: replace with insertion sort for efficiency.
//  // FIXME: break out sort as separate subroutine.
//  uint8_t n = N;
//  do
//    {
//    uint8_t newn = 0;
//    for(uint8_t i = 0; ++i < n; )
//      {
//      const int c0 = copy[i-1];
//      const int c1 = copy[i];
//      if(c0 > c1)
//         {
//         copy[i] = c0;
//         copy[i-1] = c1;
//         newn = i;
//         }
//      }
//    n = newn;
//    } while(0 != n);
//#if 0 && defined(DEBUG)
//DEBUG_SERIAL_PRINT_FLASHSTRING("sorted: ");
//for(uint8_t i = 0; i < N; ++i) { DEBUG_SERIAL_PRINT(copy[i]); DEBUG_SERIAL_PRINT(' '); }
//DEBUG_SERIAL_PRINTLN();
//#endif
//  // Extract mean of interquartile range.
//  const size_t sampleSize = N/2;
//  const size_t start = N/4;
//  // Assume values will be nowhere near the extremes.
//  int sum = 0;
//  for(uint8_t i = start; i < start + sampleSize; ++i) { sum += copy[i]; }
//  // Compute rounded-up mean.
//  return((sum + sampleSize/2) / sampleSize);
//  }



// Singleton implementation/instance.
RoomTemperatureC16 TemperatureC16;

// Temperature read uses/selects one of the implementations/sensors.
int RoomTemperatureC16::read()
  {
#ifdef SENSOR_SHT21_ENABLE
  const int raw = Sensor_SHT21_readTemperatureC16();
#else
  const int raw = TMP112_readTemperatureC16();
#endif

  value = raw;
  return(value);
  }





#ifdef TEMP_POT_AVAILABLE
// Minimum change (hysteresis) enforced in 'reduced noise' version value; must be greater than 1.
// Aim to provide reasonable noise immunity, even from an ageing carbon-track pot.
// Allow reasonable remaining granularity of response, at least 10s of distinct positions (>=5 bits).
#define RN_HYST 4

// Bottom and top parts of reduced noise range reserved for forcing FROST or BOOST.
// Should be big enough to hit easily (and must be larger than RN_HYST)
// but not so big as to really constrain the temperature range or cause confusion.
#define RN_FRBO (max(8, 2*RN_HYST))

// Force a read/poll of the temperature pot and return the value sensed [0,255] (cold to hot).
// Potentially expensive/slow.
// This value has some hysteresis applied to reduce noise.
// Not thread-safe nor usable within ISRs (Interrupt Service Routines).
uint8_t TemperaturePot::read()
  {
  // No need to wait for voltage to stablise as pot top end directly driven by IO_POWER_UP.
  power_intermittent_peripherals_enable(false);
  const uint16_t tpRaw = analogueNoiseReducedRead(TEMP_POT_AIN, DEFAULT); // Vcc reference.
  power_intermittent_peripherals_disable();

#if defined(TEMP_POT_REVERSE)
  const uint16_t tp = TEMP_POT_RAW_MAX - tpRaw; // Travel is in opposite direction to natural!
#else
  const uint16_t tp = tpRaw;
#endif

  // TODO: capture entropy from changed LS bits esp if reduced-noise version doesn't change.

  // Store new raw value.
  raw = tp;

  // Capture reduced-noise value with a little hysteresis.
  const uint8_t oldValue = value;
  const uint8_t shifted = tp >> 2; // Keep signed to avoid wrap-round confusion.
  if(((shifted > oldValue) && (shifted - oldValue >= RN_HYST)) ||
     ((shifted < oldValue) && (oldValue - shifted >= RN_HYST)))
    {
    const uint8_t rn = (uint8_t) shifted;
    // Atomically store reduced-noise normalised value.
    value = rn;

    // Smart responses to adjustment/movement of temperature pot.
    // Possible to get reasonable functionality without using MODE button.
    //
    // NOTE: without ignoredFirst this will also respond to the initial position of the pot
    //   as the first reading is taken, ie may force to WARM or BAKE.
    static bool ignoredFirst;
    if(!ignoredFirst) { ignoredFirst = true; }
    else
      {
      // Force FROST mode when right at bottom of dial.
      if(rn < RN_FRBO) { setWarmModeDebounced(false); }
#ifdef SUPPORT_BAKE // IF DEFINED: this unit supports BAKE mode.
      // Start BAKE mode when dial turned up to top.
      else if(rn > (255-RN_FRBO)) { startBakeDebounced(); }
#endif
      // Force WARM mode if pot/temperature turned up.
      else if(rn > oldValue) { setWarmModeDebounced(true); }

      // Note user operation of pot.
      markUIControlUsed(); 
      }
    }

#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("Temp pot: ");
  DEBUG_SERIAL_PRINT(tp);
  DEBUG_SERIAL_PRINT_FLASHSTRING(", rn: ");
  DEBUG_SERIAL_PRINT(tempPotReducedNoise);
  DEBUG_SERIAL_PRINTLN();
#endif

  return(value);
  }

// Singleton implementation/instance.
TemperaturePot TempPot;
#endif











#ifdef ENABLE_VOICE_SENSOR
// If count meets or exceeds this threshold in one poll period then
// the room is deemed to be occupied.
// Strictly positive.
#define VOICE_DETECTION_THRESHOLD 2

// Force a read/poll of the voice level and return the value sensed.
// Thread-safe and ISR-safe.
uint8_t VoiceDetection::read()
  {
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
    {
    isDetected = ((value = count) >= VOICE_DETECTION_THRESHOLD);
    count = 0;
    }
  }

// Handle simple interrupt.
// Fast and ISR (Interrupt Service Routines) safe.
// Returns true if interrupt was successfully handled and cleared
// else another interrupt handler in the chain may be called
// to attempt to clear the interrupt.
bool VoiceDetection::handleInterruptSimple()
  {
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
    {
    // Count of voice activations since last poll, avoiding overflow.
    if((count < 255) && (++count >= VOICE_DETECTION_THRESHOLD))
      {
      // Act as soon as voice is detected.
      isDetected = true;
      markAsOccupied();
      }
    }
    // No further work to be done to 'clear' interrupt.
    return(true);
  }

// Singleton implementation/instance.
VoiceDetection Voice;
#endif




#ifdef DIRECT_MOTOR_DRIVE_V1
// Actuator/driver for direct local (radiator) valve motor control.

uint8_t ValveMotorDirectV1::read()
  {

  // TODO

  }

#if 1 && defined(ALT_MAIN_LOOP) && defined(DEBUG)
// Drive motor back and forth (toggle direction each call) just for testing/fun.
void ValveMotorDirectV1::flip()
  {
  static bool open;
  open = !open;
  motorDrive(open ? motorDriveOpening : motorDriveClosing);
  }
#endif

// Minimally wiggles the motor to give tactile feedback and/or show to be working.
// Does not itself track movement against shaft encoder, etc, or check for stall.
// May take a significant fraction of a second.
// Finishes with the motor turned off.
void ValveMotorDirectV1::wiggle()
  {
  motorDrive(motorOff);
  motorDrive(motorDriveOpening);
  nap(WDTO_120MS);
  motorDrive(motorDriveClosing);
  nap(WDTO_120MS);
  motorDrive(motorOff);
  }

// Turn motor off, or on for a given drive direction.
// This routine is very careful to avoid setting outputs into any illegal/'bad' state.
// Sets flags accordingly.
// Does not provide any monitoring of stall, position encoding, etc.
// May take significant time (~150ms) to gently stop motor.
void ValveMotorDirectV1::motorDrive(const motor_drive dir)
  {
  // *** MUST NEVER HAVE L AND R LOW AT THE SAME TIME else board may be destroyed at worst. ***
  // Operates as quickly as reasonably possible, eg to move to stall detection quickly...
  // TODO: consider making atomic to block some interrupt-related accidents...
  // TODO: note that the mapping between L/R and open/close not yet defined.
  switch(dir)
    {
    case motorDriveOpening:
      {
      fastDigitalWrite(MOTOR_DRIVE_ML, HIGH); // Pull one side high immediately *FIRST* for safety.
      nap(WDTO_120MS); // Let H-bridge respond and settle, and motor slow down.
      pinMode(MOTOR_DRIVE_MR, OUTPUT); // Ensure that the LOW side is an output.
      fastDigitalWrite(MOTOR_DRIVE_MR, LOW); // Pull other side side low after.
      nap(WDTO_15MS); // Let H-bridge respond and settle.
//LED_HEATCALL_ON();
//LED_UI2_OFF();
      break; // Fall through to common case.
      }

    case motorDriveClosing:
      {
      fastDigitalWrite(MOTOR_DRIVE_MR, HIGH); // Pull one side high immediately *FIRST* for safety.
      nap(WDTO_120MS); // Let H-bridge respond and settle, and motor slow down.
      pinMode(MOTOR_DRIVE_ML, OUTPUT); // Ensure that the LOW side is an output.
      fastDigitalWrite(MOTOR_DRIVE_ML, LOW); // Pull other side side low after.
      nap(WDTO_15MS); // Let H-bridge respond and settle.
//LED_HEATCALL_OFF();
//LED_UI2_ON();
      break; // Fall through to common case.
      }

    case motorOff: default: // Explicit off, and default for safety.
      {
      // Everything off...
      fastDigitalWrite(MOTOR_DRIVE_MR, HIGH); // Belt and braces force pin logical output state high.
      pinMode(MOTOR_DRIVE_MR, INPUT_PULLUP); // Switch to weak pull-up; slow but possibly marginally safer.
      nap(WDTO_15MS); // Let H-bridge respond and settle.
      fastDigitalWrite(MOTOR_DRIVE_ML, HIGH); // Belt and braces force pin logical output state high.
      pinMode(MOTOR_DRIVE_ML, INPUT_PULLUP); // Switch to weak pull-up; slow but possibly marginally safer.
      nap(WDTO_15MS); // Let H-bridge respond and settle.
      motorDriveStatus = motorOff; // Ensure value state even if 'dir' invalid.
      return; // Return, not fall through.
      }
    }

  // If state has changed to new 'active' state,
  // force both lines to outputs (which may be relatively slow)
  // and update this instance's state.
  if(motorDriveStatus != dir) 
    {
    pinMode(MOTOR_DRIVE_ML, OUTPUT);
    pinMode(MOTOR_DRIVE_MR, OUTPUT);
    motorDriveStatus = dir;
    }
  }

// Singleton implementation/instance.
ValveMotorDirectV1 ValveDirect;
#endif

