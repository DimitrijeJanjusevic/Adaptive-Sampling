# Adaptive Sampling IoT System with FreeRTOS on ESP32

## Project Overview

This project implements an adaptive sampling IoT system on an ESP32 using FreeRTOS. The system intelligently adjusts its sampling frequency based on real-time FFT analysis of the input signal, demonstrating significant reductions in sampling activity and network data transmission.

### Key Features

- **Adaptive Sampling**: Automatically adjusts sampling rate based on detected signal frequency
- **FFT-based Frequency Detection**: Real-time spectral analysis to find dominant frequency
- **FreeRTOS Multi-tasking**: Three independent tasks with priority-based scheduling
- **MQTT over WiFi**: Sends aggregated data to edge server
- **LoRaWAN Simulation**: Cloud communication simulation (architectural placeholder)
- **Performance Metrics**: Energy savings estimation, data reduction, latency measurement

### Performance Summary

| Metric | Value |
|--------|-------|
| Initial Sampling Rate | 100 Hz |
| Adapted Sampling Rate | 12.56 Hz |
| Estimated Energy Savings | 87.25% |
| Raw Data (100 windows) | 200,000 bytes |
| MQTT Data Sent | 14,000 bytes |
| Data Reduction | 93% |
| Per-Window Processing Latency | ~45 ms |
| FFT Computation Time | ~960 us |

---

## System Architecture

### FreeRTOS Task Structure

The system uses three FreeRTOS tasks with different priorities:

**Sampling Task (Priority 3 - Highest)**
- Generates the test signal
- Samples at current rate
- Fills 5-second window buffer
- Sends buffer descriptor to processing queue

**Processing Task (Priority 2 - Medium)**
- Receives complete windows from queue
- Computes window average
- Runs FFT to find dominant frequency
- Calculates next sampling rate (Nyquist × 2.5)
- Sends rate command back to sampling task
- Sends results to communication queue

**Communication Task (Priority 1 - Lowest)**
- Receives results from queue
- Publishes MQTT messages to edge server
- Simulates LoRaWAN to cloud (architectural placeholder)
- Tracks performance metrics
- Prints final performance report

### Data Flow

1. Sampling Task collects samples for 5 seconds at current rate
2. Complete window sent to Processing Task via queue (only buffer ID, not data)
3. Processing Task computes average and FFT
4. New sampling rate sent back to Sampling Task
5. Results sent to Communication Task
6. Communication Task publishes via MQTT and logs performance

### Double Buffering

Two static buffers prevent data copying:
- Sampling Task fills one buffer while Processing Task reads the other
- Only buffer IDs are passed through queues (zero-copy)
- Mutex protects buffer status changes
- Yield function

---

## Signal Model

The system generates a synthetic test signal (virtual sensor):

```
s(t) = 2·sin(2π·3·t) + 4·sin(2π·5·t)
```

**Signal components:**
- 3 Hz component with amplitude 2
- 5 Hz component with amplitude 4 (dominant)

The FFT correctly detects the dominant frequency at approximately 5 Hz, triggering adaptation to approximately 12.5 Hz.

---

## Adaptive Sampling Method

### Frequency Detection

Every 5 seconds, the system:
1. Collects samples at current rate
2. Runs FFT to find dominant frequency
3. Applies low-pass filter to prevent oscillation (α = 0.7)
4. Uses hysteresis threshold of 2 Hz to avoid rapid changes

### Rate Adaptation Formula

```
new_rate = dominant_frequency × 2.5
```

**Why 2.5× Nyquist instead of 2×?**
- Theoretical minimum is 2× (Nyquist)
- 2.5× provides safety margin for non-ideal signals
- Accounts for FFT frequency resolution limitations
- Compensates for potential signal variations between windows

### Rate Change Protection

- Minimum rate: 10 Hz
- Maximum rate: 100 Hz
- Change threshold: 2 Hz (prevents oscillation)
- Low-pass filter: smoothes frequency estimates

---

## Communication Path

### Edge Server (MQTT over WiFi)

The ESP32 publishes each window's results to an MQTT topic:

**Topic:** `adaptive-sampling/window`

**Message Format (JSON):**
```json
{
  "avg": -0.067220,
  "dom_freq": 5.102,
  "sample_rate": 12.559,
  "next_rate": 12.755,
  "fft_us": 836,
  "latency_ms": 43.717,
  "samples": 62,
  "fft_len": 32
}
```

### Cloud Communication (LoRaWAN Simulation)

**Note:** LoRaWAN transmission is simulated due to lack of LoRa hardware. The code contains the line:

```cpp
Serial.println("[LORAWAN] Sending to cloud (simulated)");
```

This serves as an architectural placeholder demonstrating where real LoRaWAN transmission would integrate. A production implementation would replace this with actual LoRaWAN library calls to The Things Network (TTN) using a SX1276 or similar module.

---

## Maximum Sampling Frequency

According to Espressif's official ADC documentation:

| Condition | Maximum Sampling Rate |
|-----------|----------------------|
| ADC DMA mode, Wi-Fi disabled | ~2 MHz (theoretical upper bound) |
| Practical operation with Wi-Fi enabled | ~100 samples/second |

This project uses a virtual sensor and Wi-Fi-based MQTT edge path. The practical rate (100 Hz baseline) is treated as the relevant reference for system evaluation, while the 2 MHz figure is cited as the hardware's theoretical maximum.

---

## Performance Results

### Serial Monitor Output Example

```
==========================================
   ADAPTIVE SAMPLING SYSTEM
   Submission Version
==========================================

Connecting to WiFi: TIM FWA-HRX8
.....
WiFi connected!
ESP32 IP address: 192.168.1.xxx

========== WINDOW RESULTS ==========
Samples: 500 @ 100.00 Hz
FFT length: 256 points
Dominant frequency: 5.01 Hz
Next rate: 12.53 Hz
FFT time: 8234 us
Latency: 43.72 ms
Data reduction: 92.50%
MQTT publish OK
=====================================

==========================================
RATE UPDATE: 100.00 Hz → 12.53 Hz
Energy savings: 87.47%
==========================================

========== WINDOW RESULTS ==========
Samples: 62 @ 12.56 Hz
FFT length: 32 points
Dominant frequency: 5.10 Hz
Next rate: 12.75 Hz
FFT time: 814 us
Latency: 45.28 ms
Data reduction: 67.74%
MQTT publish OK
=====================================
```

### Final Performance Report

```
==========================================
        FINAL PERFORMANCE REPORT
==========================================
1. ENERGY SAVINGS
   Initial rate: 100.00 Hz
   Average adapted rate: 12.56 Hz
   Average energy saved: 87.25%

2. DATA VOLUME
   Raw data if oversampled: 200000 bytes
   MQTT data sent (adaptive): 14000 bytes
   Data reduction: 93.00%

3. END-TO-END LATENCY
   Minimum: 41.97 ms
   Maximum: 14728.34 ms

4. PER-WINDOW EXECUTION TIME
   Average FFT computation: 958.97 us
   Windows processed: 100

5. FFT PERFORMANCE
   At 100 Hz: 500 samples → 256-point FFT, ~8 ms
   At 12.5 Hz: 62 samples → 32-point FFT, ~0.8 ms
==========================================
```

### FFT Performance Details

The FFT uses dynamic power-of-2 sizing based on the number of samples:
- At 100 Hz: 500 samples → 256-point FFT, approximately 8 milliseconds
- At 12.5 Hz: 62 samples → 32-point FFT, approximately 0.8 milliseconds

---

## Energy and Communication Analysis

### Energy Savings Estimation

Energy savings are estimated from the reduction in sampling frequency. This is a theoretical estimate based on sampling activity, not direct current measurement.

**Formula:** `Energy Saved = (1 - Adapted Rate / Initial Rate) × 100%`

**Calculation:** `(1 - 12.56 / 100) × 100% = 87.44%` (measured: 87.25%)

### Data Reduction Calculation

| Scenario | Data per 5-second window | Total for 100 windows |
|----------|-------------------------|----------------------|
| Raw data (if oversampled) | 500 samples × 4 bytes = 2000 bytes | 200,000 bytes |
| Adaptive (MQTT) | ~140 bytes per JSON message | 14,000 bytes |
| **Reduction** | **93%** | **93%** |

### Latency Measurement Definitions

Two latency measurements are reported:

| Metric | Definition | Typical Value |
|--------|------------|---------------|
| **Per-Window Processing Latency** | Time from window acquisition completion to MQTT publish ready | ~45 ms |
| **Cumulative End-to-End Latency** | Average across all windows including WiFi/MQTT transmission delays | ~2656 ms |

The large difference is explained by occasional WiFi reconnection events and MQTT broker delays, which cause spikes (e.g., 4858 ms, 14728 ms). These outliers significantly impact the cumulative average while per-window processing remains fast.

### Theoretical Energy Savings with Light Sleep

The ESP32 consumes different amounts of current in different states:

| State | Current Consumption | Description |
|-------|--------------------|-------------|
| Active (CPU running) | ~80-100 mA | Sampling, FFT, MQTT |
| WiFi Transmitting | ~120-160 mA | Sending MQTT messages |
| Light Sleep | ~0.8-2 mA | CPU paused, can wake on timer |
| Deep Sleep | ~10-150 µA | Everything off, needs reset to wake |

**With light sleep between samples (12.5 Hz):**
- Sample duration: ~100 µs
- Time between samples: 80,000 µs (80 ms)
- Duty cycle: 100 / 80,000 = 0.125%

Average current = (100 mA × 0.0001s + 1 mA × 0.0799s) / 0.08s = 1.12 mA

**Energy saving vs continuous running:** `(100 - 1.12) / 100 × 100% = 98.88%`

**Note:** Light sleep would interfere with WiFi connectivity. A production system would use batch transmission with deep sleep instead.

### Current Limitations

| Limitation | Description | Impact |
|------------|-------------|--------|
| Busy-wait sampling | Uses `while(micros() < target)` with `taskYIELD()` | Higher CPU usage than timer interrupts |
| LoRaWAN simulation | No real LoRa hardware | Cloud communication not truly implemented |
| 32-bit timestamps | `micros()` wraps every 71 minutes | Long runs may have timing errors |
| No light sleep | WiFi must stay connected | Higher power consumption |
| Synthetic signal | No physical sensor | Real-world validation not performed |

### Future Improvements

1. **Timer Interrupt Sampling**: Replace busy-wait with hardware timer for precise, low-CPU sampling
2. **Real LoRaWAN**: Add SX1276 module and integrate with The Things Network
3. **Batch Transmission**: Collect multiple windows, send once per minute, deep sleep between
4. **64-bit Timestamps**: Prevent wraparound for long-duration runs
5. **Physical Sensor Integration**: Replace synthetic signal with actual ADC readings
6. **Current Measurement**: Add external current monitoring for true energy validation
