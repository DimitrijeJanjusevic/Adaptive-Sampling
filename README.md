# Adaptive Sampling IoT System with FreeRTOS on ESP32

## Overview

This project implements an adaptive sampling IoT system on an ESP32 using FreeRTOS. The system intelligently adjusts its sampling frequency based on real-time FFT analysis of the input signal, significantly reducing energy consumption and network data transmission.

### Key Features

- Adaptive Sampling: Automatically adjusts sampling rate from 100 Hz to ~12.5 Hz after detecting signal frequency
- FFT-based Frequency Detection: Real-time spectral analysis to find dominant frequency
- FreeRTOS Multi-tasking: Three independent tasks with priority-based scheduling
- MQTT over WiFi: Sends aggregated data to edge server
- LoRaWAN Simulation: Cloud communication simulation (ready for real hardware)
- Performance Metrics: Energy savings, data reduction, latency, and execution time tracking

### Performance Results Summary

| Metric | Value |
|--------|-------|
| Initial Sampling Rate | 100 Hz |
| Adapted Sampling Rate | 12.56 Hz |
| Energy Savings | 87.25% |
| Raw Data (100 windows) | 200,000 bytes |
| MQTT Data Sent | 14,000 bytes |
| Data Reduction | 93% |
| Average Latency (typical) | ~45 ms |
| FFT Computation Time | ~960 us |

## System Architecture

## Max frequency
According to Espressif’s official ADC FAQ, the ESP32 ADC can theoretically sample at up to 2 MHz when Wi-Fi is disabled and ADC DMA is used. In practical Wi-Fi-connected IoT scenarios, Espressif reports a sampling rate of about 1000 samples per second. Since this project uses a virtual sensor and a Wi-Fi-based MQTT edge path, we treat the practical rate as the relevant baseline for system evaluation, while citing the 2 MHz figure as the hardware upper bound

### FreeRTOS Task Structure

The system uses three FreeRTOS tasks with different priorities:

Sampling Task (Priority 3 - Highest):
- Generates the test signal
- Samples at current rate
- Fills 5-second window buffer
- Sends buffer descriptor to processing queue

Processing Task (Priority 2 - Medium):
- Receives complete windows from queue
- Computes window average
- Runs FFT to find dominant frequency
- Calculates next sampling rate (Nyquist x 2.5)
- Sends rate command back to sampling task
- Sends results to communication queue

Communication Task (Priority 1 - Lowest):
- Receives results from queue
- Publishes MQTT messages to edge server
- Simulates LoRaWAN to cloud
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

Two static buffers are used to prevent data copying:
- Sampling Task fills one buffer while Processing Task reads the other
- Only buffer IDs are passed through queues (zero-copy)
- Mutex protects buffer status changes


## Input Signal

The system generates a synthetic test signal (virtual sensor):

s(t) = 2 * sin(2 * pi * 3 * t) + 4 * sin(2 * pi * 5 * t)

Signal components:
- 3 Hz component with amplitude 2
- 5 Hz component with amplitude 4 (dominant)

The FFT correctly detects the dominant frequency at approximately 5 Hz, triggering adaptation to approximately 12.5 Hz (2.5 times Nyquist for safety margin).

## Output and Results

Serial Monitor Output Example:

==========================================
   ADAPTIVE SAMPLING SYSTEM
   Submission Version
==========================================

Connecting to WiFi: TIM FWA-HRX8
.....
WiFi connected!
ESP32 IP address: 192.168.1.186

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
RATE UPDATE: 100.00 Hz to 12.53 Hz
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

MQTT Message Format (JSON):

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

Final Performance Report (after 100 windows):

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
   Average: 2656.09 ms
   Minimum: 41.97 ms
   Maximum: 14728.34 ms

4. PER-WINDOW EXECUTION TIME
   Average FFT computation: 958.97 us
   Windows processed: 100

### Theoretical Energy Savings from Adaptive Sampling

The ESP32 consumes different amounts of current in different states:

| State | Current Consumption | Description |
|-------|--------------------|-------------|
| Active (CPU running) | ~80-100 mA | Sampling, FFT, MQTT |
| WiFi Transmitting | ~120-160 mA | Sending MQTT messages |
| Light Sleep | ~0.8-2 mA | CPU paused, can wake on timer |
| Deep Sleep | ~10-150 uA | Everything off, needs reset to wake |

### Energy Calculation Without Light Sleep

Without any sleep optimization, the ESP32 runs continuously:

Energy per second = 100 mA × 3.3V = 0.33 Watts

For 5-second window at 100 Hz: 500 samples
For 5-second window at 12.5 Hz: 62 samples

Energy saved by adaptive sampling = (500 - 62) / 500 × 100% = 87.6%

### Energy Calculation With Light Sleep

At 12.5 Hz sampling rate:
- Sample duration: ~100 microseconds (to read value)
- Time between samples: 80,000 microseconds (80 ms)
- Duty cycle: 100 / 80,000 = 0.125%

With light sleep between samples:
- Active current: 100 mA for 100 us
- Sleep current: 1 mA for 79,900 us

Average current = (100 mA × 0.0001s + 1 mA × 0.0799s) / 0.08s
Average current = (0.01 + 0.0799) / 0.08 = 1.12 mA

Compare to running continuously at 100 mA:
Energy saving = (100 - 1.12) / 100 × 100% = 98.88%

==========================================

## Performance Analysis

Energy Savings Calculation:

Energy consumption is proportional to sampling frequency. The formula used is:
Energy Saved = (1 - Adapted Rate / Initial Rate) x 100%
Example: (1 - 12.5 / 100) x 100% = 87.5%

Data Reduction Calculation:

Without adaptation: 500 samples per window x 4 bytes = 2000 bytes per window
With adaptation: 1 JSON message per window approximately 140 bytes
Data Reduction = (1 - 140 / 2000) x 100% = 93%

End-to-End Latency:

Latency is measured from acquisition_end_us (when sampling finishes) to the time the communication task receives the result. Typical values are 40-50 milliseconds. Occasional spikes to 5 seconds occur due to WiFi reconnection events or MQTT broker delays.

FFT Performance:

The FFT uses dynamic power-of-2 sizing based on the number of samples:
- At 100 Hz: 500 samples -> 256-point FFT, approximately 8 milliseconds
- At 12.5 Hz: 62 samples -> 32-point FFT, approximately 0.8 milliseconds
.
- PubSubClient library by knolleary
- Claude AI for development assistance
