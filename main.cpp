
#include <Arduino.h>
#include "arduinoFFT.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <WiFi.h>
#include <PubSubClient.h>

// CONFIGURATION 
#define WINDOW_DURATION_MS 5000       // Fixed 5-second window
#define MAX_SAMPLE_RATE 100.0         // Maximum sampling rate 
#define MIN_SAMPLE_RATE 10.0          // Minimum sampling rate 
#define MAX_FFT_SIZE 256              // Maximum FFT size 
#define MAX_SAMPLES_PER_WINDOW 500     // 100 Hz * 5 sec = 500 max

// Rate adaptation parameters
#define RATE_CHANGE_THRESHOLD 2.0     // Minimum change to adapt 
#define FREQ_FILTER_ALPHA 0.7         

//TASK CONFIGURATION 
#define SAMPLING_TASK_PRIORITY 3
#define PROCESSING_TASK_PRIORITY 2
#define COMMUNICATION_TASK_PRIORITY 1

#define SAMPLING_TASK_STACK 4096
#define PROCESSING_TASK_STACK 8192
#define COMMUNICATION_TASK_STACK 4096

// BUFFER CONFIGURATION
#define NUM_BUFFERS 2
//WIFI / MQTT CONFIG 
const char* WIFI_SSID = "TIM FWA-HRX8";
const char* WIFI_PASSWORD = "4wUi797p4bRv";
//EVALUATION
unsigned long total_raw_samples_if_oversampled = 0;
unsigned long total_mqtt_bytes_sent = 0;
unsigned long total_fft_time_us = 0;
float min_latency = 999999;
float max_latency = 0;
float total_energy_saved_percent = 0;
unsigned long total_window_process_time_us = 0;
float total_latency_ms = 0;      
int latency_samples = 0;           

//IPv4 address 
const char* MQTT_BROKER = "192.168.1.131";
const int MQTT_PORT = 1883;
const char* MQTT_TOPIC = "adaptive-sampling/window";

WiFiClient esp_wifi_client;
PubSubClient mqttClient(esp_wifi_client);

// DATA STRUCTURES 
typedef enum {
    BUFFER_EMPTY = 0,
    BUFFER_FILLING = 1,
    BUFFER_FULL = 2,
    BUFFER_PROCESSING = 3
} BufferStatus;

// Buffer descriptor
typedef struct {
    uint8_t buffer_id;
    uint32_t acquisition_start_us;
    uint32_t acquisition_end_us;
    uint16_t sample_count;
    float sample_rate_used;
} BufferDescriptor;

// Result structure
typedef struct {
    float window_average;
    float dominant_frequency;
    float sample_rate_used;
    float next_sample_rate;
    uint32_t fft_time_us;
    uint16_t sample_count;
    uint16_t fft_length_used;
    uint8_t buffer_id;
    uint32_t acquisition_end_us;
} ProcessedResult;

// Rate command
typedef struct {
    float new_sample_rate;
} RateCommand;

// GLOBAL BUFFERS
float buffers[NUM_BUFFERS][MAX_SAMPLES_PER_WINDOW];
BufferStatus buffer_status[NUM_BUFFERS];

// FFT buffers
double fft_real[MAX_FFT_SIZE];
double fft_imag[MAX_FFT_SIZE];

// SYNCHRONIZATION
SemaphoreHandle_t buffer_mutex = NULL;
SemaphoreHandle_t rate_mutex = NULL;
QueueHandle_t buffer_ready_queue = NULL;
QueueHandle_t result_queue = NULL;
QueueHandle_t rate_command_queue = NULL;

// GLOBAL VARIABLES 
float current_sample_rate = MAX_SAMPLE_RATE;
unsigned long total_windows_processed = 0;
unsigned long total_samples_collected = 0;
unsigned long global_time_offset_us = 0;
float filtered_freq = 5.0;

// Error counters for debugging
unsigned long queue_send_errors = 0;


// Function prototypes 
int largestPowerOfTwo(int n);
float generateSignal(float t);
float getCurrentSampleRate();
void setCurrentSampleRate(float new_rate);
float findDominantFrequency(float* samples, int sample_count, float sample_rate, int* fft_len_used);

void samplingTask(void* pvParameters);
void processingTask(void* pvParameters);
void communicationTask(void* pvParameters);
void printFinalReport();

bool createTask(TaskFunction_t taskCode, const char* name, uint32_t stackDepth,
                void* parameters, UBaseType_t priority, int core);
QueueHandle_t createQueue(UBaseType_t length, UBaseType_t item_size, const char* name);

void connectWiFi();
void reconnectMQTT();

  
int largestPowerOfTwo(int n) {
    int p = 1;
    while ((p << 1) <= n && (p << 1) <= MAX_FFT_SIZE) {
        p <<= 1;
    }
    return p;
}

// Signal generator
float generateSignal(float t) {
    return 2.0 * sin(2.0 * PI * 3.0 * t) +
           4.0 * sin(2.0 * PI * 5.0 * t);
}

// thread safe function
float getCurrentSampleRate() {
    float rate;
    xSemaphoreTake(rate_mutex, portMAX_DELAY);
    rate = current_sample_rate;
    xSemaphoreGive(rate_mutex);
    return rate;
}

void setCurrentSampleRate(float new_rate) {
    xSemaphoreTake(rate_mutex, portMAX_DELAY);
    if(fabs(new_rate - current_sample_rate) > RATE_CHANGE_THRESHOLD) {
        if(new_rate >= MIN_SAMPLE_RATE && new_rate <= MAX_SAMPLE_RATE) {
            Serial.println("==========================================");
            Serial.print("RATE UPDATE: ");
            Serial.print(current_sample_rate);
            Serial.print(" Hz → ");
            Serial.print(new_rate);
            Serial.println(" Hz");
            
            float energy_saved = (1.0 - new_rate / MAX_SAMPLE_RATE) * 100.0;
            Serial.print("Energy savings: ");
            Serial.print(energy_saved);
            Serial.println("%");
            Serial.println("==========================================");
            
            current_sample_rate = new_rate;
        }
    }
    xSemaphoreGive(rate_mutex);
}

// FFT FUNCTION 
float findDominantFrequency(float* samples, int sample_count, float sample_rate, int* fft_len_used) {
    int fft_len = largestPowerOfTwo(sample_count);
    *fft_len_used = fft_len;
    
    // Fill FFT buffers
    for(int i = 0; i < fft_len; i++) {
        fft_real[i] = (double)samples[i];
        fft_imag[i] = 0.0;
    }
    // Zero padding for remaining
    for(int i = fft_len; i < MAX_FFT_SIZE; i++) {
        fft_real[i] = 0.0;
        fft_imag[i] = 0.0;
    }
    
    ArduinoFFT<double> FFT = ArduinoFFT<double>(fft_real, fft_imag, fft_len, sample_rate);
    
    FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
    FFT.compute(FFTDirection::Forward);
    FFT.complexToMagnitude();
    
    // Find peak 
    double max_magnitude = 0;
    int peak_bin = 1;
    for(int i = 1; i < (fft_len / 2); i++) {
        if(fft_real[i] > max_magnitude) {
            max_magnitude = fft_real[i];
            peak_bin = i;
        }
    }
    
    return (peak_bin * sample_rate) / fft_len;
}

// SAMPLING TASK
void samplingTask(void* pvParameters) {
    BufferDescriptor desc;
    RateCommand rate_cmd;
    float sample_interval_us;
    int samples_needed;
    float local_rate;
    
    global_time_offset_us = micros();
    
    while(1) {
        // Check for rate change
        if(xQueueReceive(rate_command_queue, &rate_cmd, 0) == pdTRUE) {
            setCurrentSampleRate(rate_cmd.new_sample_rate);
        }
        
        local_rate = getCurrentSampleRate();
        samples_needed = (int)(local_rate * (WINDOW_DURATION_MS / 1000.0));
        if(samples_needed > MAX_SAMPLES_PER_WINDOW) {
            samples_needed = MAX_SAMPLES_PER_WINDOW;
        }
        
        sample_interval_us = 1000000.0 / local_rate;
        
        // Find empty buffer
        xSemaphoreTake(buffer_mutex, portMAX_DELAY);
        int buffer_to_fill = -1;
        for(int i = 0; i < NUM_BUFFERS; i++) {
            if(buffer_status[i] == BUFFER_EMPTY) {
                buffer_to_fill = i;
                buffer_status[i] = BUFFER_FILLING;
                break;
            }
        }
        xSemaphoreGive(buffer_mutex);
        
        if(buffer_to_fill == -1) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        // Collect samples
        uint32_t window_start_us = micros();
        desc.buffer_id = buffer_to_fill;
        desc.sample_rate_used = local_rate;
        desc.acquisition_start_us = window_start_us;
        
        for(int i = 0; i < samples_needed; i++) {
            uint32_t target_us = window_start_us + (i * (uint32_t)sample_interval_us);
            
            while(micros() < target_us) {
                taskYIELD();
            }
            
            float t = (micros() - global_time_offset_us) / 1000000.0;
            buffers[buffer_to_fill][i] = generateSignal(t);
            total_samples_collected++;
        }
        
        desc.acquisition_end_us = micros();
        desc.sample_count = samples_needed;
        
        xSemaphoreTake(buffer_mutex, portMAX_DELAY);
        buffer_status[buffer_to_fill] = BUFFER_FULL;
        xSemaphoreGive(buffer_mutex);
        
        
        if(xQueueSend(buffer_ready_queue, &desc, portMAX_DELAY) != pdTRUE) {
            queue_send_errors++;
            Serial.println("ERROR: buffer_ready_queue send failed");
        }
    }
}

// PROCESSING TASK 
void processingTask(void* pvParameters) {
    BufferDescriptor desc;
    ProcessedResult result;
    RateCommand rate_cmd;
    float* samples;
    float window_avg, dominant_freq, next_rate;
    uint32_t fft_start;
    int fft_len_used;
    
    while(1) {
        if(xQueueReceive(buffer_ready_queue, &desc, portMAX_DELAY) == pdTRUE) {
            
            xSemaphoreTake(buffer_mutex, portMAX_DELAY);
            if(buffer_status[desc.buffer_id] != BUFFER_FULL) {
                xSemaphoreGive(buffer_mutex);
                continue;
            }
            buffer_status[desc.buffer_id] = BUFFER_PROCESSING;
            samples = buffers[desc.buffer_id];
            xSemaphoreGive(buffer_mutex);
            
            // average
            float sum = 0;
            for(int i = 0; i < desc.sample_count; i++) {
                sum += samples[i];
            }
            window_avg = sum / desc.sample_count;
            
            // FFT
            fft_start = micros();
            dominant_freq = findDominantFrequency(samples, desc.sample_count, 
                                                   desc.sample_rate_used, &fft_len_used);
            result.fft_time_us = micros() - fft_start;
            
            // Filter and adapt
            filtered_freq = FREQ_FILTER_ALPHA * filtered_freq + 
                           (1 - FREQ_FILTER_ALPHA) * dominant_freq;
            
            next_rate = filtered_freq * 2.5;
            if(next_rate > MAX_SAMPLE_RATE) next_rate = MAX_SAMPLE_RATE;
            if(next_rate < MIN_SAMPLE_RATE) next_rate = MIN_SAMPLE_RATE;
            
            
            rate_cmd.new_sample_rate = next_rate;
            if(xQueueSend(rate_command_queue, &rate_cmd, portMAX_DELAY) != pdTRUE) {
                queue_send_errors++;
                Serial.println("ERROR: rate_command_queue send failed");
            }
            
            // Prepare result
            result.window_average = window_avg;
            result.dominant_frequency = dominant_freq;
            result.sample_rate_used = desc.sample_rate_used;
            result.next_sample_rate = next_rate;
            result.sample_count = desc.sample_count;
            result.fft_length_used = fft_len_used;
            result.buffer_id = desc.buffer_id;
            result.acquisition_end_us = desc.acquisition_end_us;
            
          
            if(xQueueSend(result_queue, &result, portMAX_DELAY) != pdTRUE) {
                queue_send_errors++;
                Serial.println("ERROR: result_queue send failed");
            }
            
            total_windows_processed++;
            
            xSemaphoreTake(buffer_mutex, portMAX_DELAY);
            buffer_status[desc.buffer_id] = BUFFER_EMPTY;
            xSemaphoreGive(buffer_mutex);
        }
    }
}

// COMMUNICATION TASK

void communicationTask(void* pvParameters) {
    ProcessedResult result;
    static unsigned long last_perf_report = 0;
    static float total_energy_saved = 0;
    // Note: total_latency_ms and latency_samples are now GLOBAL variables
    
    while(1) {

        if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
            mqttClient.loop();
        }

        if(xQueueReceive(result_queue, &result, pdMS_TO_TICKS(200)) == pdTRUE) {
            
            // latency calculatin
            uint32_t now_us = micros();
            float latency_ms = (float)(now_us - result.acquisition_end_us) / 1000.0;
            
            // UPDATE PERFORMANCE METRICS
            total_latency_ms += latency_ms;
            latency_samples++;
            
            // Track min/max latency
            if(latency_ms < min_latency) min_latency = latency_ms;
            if(latency_ms > max_latency) max_latency = latency_ms;
            
            // Track FFT time
            total_fft_time_us += result.fft_time_us;
            
            // Print results
            Serial.println("\n========== WINDOW RESULTS ==========");
            Serial.print("Samples: ");
            Serial.print(result.sample_count);
            Serial.print(" @ ");
            Serial.print(result.sample_rate_used);
            Serial.println(" Hz");
            Serial.print("FFT length: ");
            Serial.print(result.fft_length_used);
            Serial.println(" points");
            Serial.print("Average: ");
            Serial.println(result.window_average, 6);
            Serial.print("Dominant frequency: ");
            Serial.print(result.dominant_frequency);
            Serial.println(" Hz");
            Serial.print("Next rate: ");
            Serial.print(result.next_sample_rate);
            Serial.println(" Hz");
            Serial.print("FFT time: ");
            Serial.print(result.fft_time_us);
            Serial.println(" us");
            Serial.print("Latency: ");
            Serial.print(latency_ms);
            Serial.println(" ms");
            
            // Data reduction calculation
            float raw_bytes = result.sample_count * sizeof(float);
            float mqtt_bytes = 80;
            float reduction = (1.0 - mqtt_bytes / raw_bytes) * 100.0;
            Serial.print("Data reduction: ");
            Serial.print(reduction);
            Serial.println("%");
            
            // Track bytes sent (approximate)
            total_mqtt_bytes_sent += 140;  // Average message size
            total_raw_samples_if_oversampled += 500;  // 100 Hz * 5 sec
            
            float energy_saved = (1.0 - result.next_sample_rate / MAX_SAMPLE_RATE) * 100.0;
            total_energy_saved_percent += energy_saved;
            
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("WiFi disconnected, reconnecting...");
                connectWiFi();
                mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
                mqttClient.setBufferSize(512);
                mqttClient.setKeepAlive(20);
            }

            if (!mqttClient.connected()) {
                reconnectMQTT();
            }

            char payload[256];
            snprintf(payload, sizeof(payload),
                     "{\"avg\":%.6f,\"dom_freq\":%.3f,\"sample_rate\":%.3f,"
                     "\"next_rate\":%.3f,\"fft_us\":%lu,\"latency_ms\":%.3f,"
                     "\"samples\":%u,\"fft_len\":%u}",
                     result.window_average,
                     result.dominant_frequency,
                     result.sample_rate_used,
                     result.next_sample_rate,
                     (unsigned long)result.fft_time_us,
                     latency_ms,
                     result.sample_count,
                     result.fft_length_used);

            if (mqttClient.publish(MQTT_TOPIC, payload)) {
                Serial.println("MQTT publish OK");
                Serial.print("Topic: ");
                Serial.println(MQTT_TOPIC);
                Serial.print("Payload: ");
                Serial.println(payload);
            } else {
                Serial.println("ERROR: MQTT publish failed");
            }
            
            Serial.println("[LORAWAN] Sending to cloud (simulated)");
            Serial.println("=====================================\n");
            
            // Performance summary every 30 seconds
            if(millis() - last_perf_report >= 30000) {
                Serial.println("\n********** PERFORMANCE SUMMARY **********");
                Serial.print("Windows: ");
                Serial.println(total_windows_processed);
                Serial.print("Samples: ");
                Serial.println(total_samples_collected);
                Serial.print("Avg energy savings: ");
                Serial.println(total_energy_saved / total_windows_processed);
                Serial.print("Avg latency: ");
                Serial.println(total_latency_ms / latency_samples);
                Serial.print("Current rate: ");
                Serial.print(getCurrentSampleRate());
                Serial.println(" Hz");
                
                if(queue_send_errors > 0) {
                    Serial.print("Queue send errors: ");
                    Serial.println(queue_send_errors);
                }
                Serial.println("****************************************\n");
                last_perf_report = millis();

               
            }
            if(total_windows_processed > 0 && total_windows_processed % 10 == 0) {
                printFinalReport();
            }
        }
         
    }
}

// TASK CREATION HELPER 
bool createTask(TaskFunction_t taskCode, const char* name, uint32_t stackDepth, 
                void* parameters, UBaseType_t priority, int core) {
    BaseType_t result = xTaskCreatePinnedToCore(taskCode, name, stackDepth, 
                                                 parameters, priority, NULL, core);
    if(result != pdPASS) {
        Serial.print("FAILED: ");
        Serial.println(name);
        return false;
    }
    Serial.print("OK: ");
    Serial.println(name);
    return true;
}

// QUEUE CREATION HELPER
QueueHandle_t createQueue(UBaseType_t length, UBaseType_t item_size, const char* name) {
    QueueHandle_t queue = xQueueCreate(length, item_size);
    if(queue == NULL) {
        Serial.print("FAILED to create queue: ");
        Serial.println(name);
    } else {
        Serial.print("OK: ");
        Serial.println(name);
    }
    return queue;
}

void connectWiFi() {
    Serial.print("Connecting to WiFi: ");
    Serial.println(WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    int max_attempts = 40;  // 20 seconds timeout (40 * 500ms)
    
    while (WiFi.status() != WL_CONNECTED && attempts < max_attempts) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    Serial.println();  
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connected!");
        Serial.print("ESP32 IP address: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("WiFi FAILED! Check SSID and password.");
        Serial.print("SSID used: '");
        Serial.print(WIFI_SSID);
        Serial.println("'");
    }
}

void reconnectMQTT() {
    int attempts = 0;
    while (!mqttClient.connected() && attempts < 10) {
        Serial.print("Attempting MQTT connection...");

        String clientId = "ESP32AdaptiveSampler-";
        clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

        if (mqttClient.connect(clientId.c_str())) {
            Serial.println("connected");
            Serial.print("Broker: ");
            Serial.println(MQTT_BROKER);
            Serial.print("Topic: ");
            Serial.println(MQTT_TOPIC);
            return;
        } else {
            Serial.print("failed, rc=");
            Serial.print(mqttClient.state());
            Serial.println(" retrying in 2 seconds");
            delay(2000);
            attempts++;
        }
    }
}

//  SETUP 
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n==========================================");
    Serial.println("   ADAPTIVE SAMPLING SYSTEM");
    Serial.println("   Submission Version");
    Serial.println("   Complete queue error checking");
    Serial.println("==========================================\n");
    
    connectWiFi();
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setBufferSize(512);
    mqttClient.setKeepAlive(20);
    reconnectMQTT();

    Serial.println("Creating queues:");
    buffer_ready_queue = createQueue(NUM_BUFFERS, sizeof(BufferDescriptor), "buffer_ready_queue");
    result_queue = createQueue(5, sizeof(ProcessedResult), "result_queue");
    rate_command_queue = createQueue(5, sizeof(RateCommand), "rate_command_queue");
    
    Serial.println("\nCreating mutexes:");
    buffer_mutex = xSemaphoreCreateMutex();
    rate_mutex = xSemaphoreCreateMutex();
    if(buffer_mutex == NULL || rate_mutex == NULL) {
        Serial.println("FAILED: mutex creation");
        while(1);
    }
    Serial.println("OK: mutexes created\n");
    
    // Initialize buffers
    for(int i = 0; i < NUM_BUFFERS; i++) {
        buffer_status[i] = BUFFER_EMPTY;
    }
    
    // Create tasks
    Serial.println("Creating tasks:");
    bool success = true;
    success &= createTask(samplingTask, "Sampling", SAMPLING_TASK_STACK, NULL, 
                          SAMPLING_TASK_PRIORITY, 1);
    success &= createTask(processingTask, "Processing", PROCESSING_TASK_STACK, NULL, 
                          PROCESSING_TASK_PRIORITY, 1);
    success &= createTask(communicationTask, "Communication", COMMUNICATION_TASK_STACK, NULL, 
                          COMMUNICATION_TASK_PRIORITY, 0);
    
    if(!success) {
        Serial.println("\nFATAL: Task creation failed!");
        while(1);
    }
    
    Serial.println("\nSystem ready!\n");
    Serial.print("Initial rate: ");
    Serial.print(MAX_SAMPLE_RATE);
    Serial.println(" Hz");
    Serial.print("FFT max size: ");
    Serial.print(MAX_FFT_SIZE);
    Serial.println(" (dynamic power of 2)\n");
}

// FINAL PERFORMANCE REPORT
void printFinalReport() {
    Serial.println("\n\n");
    Serial.println("==========================================");
    Serial.println("        FINAL PERFORMANCE REPORT");
    Serial.println("==========================================");
    
    if(total_windows_processed == 0) {
        Serial.println("No windows processed yet.");
        return;
    }
    
    // Energy Savings
    float avg_energy_saved = total_energy_saved_percent / total_windows_processed;
    Serial.print("1. ENERGY SAVINGS\n");
    Serial.print("   Initial rate: ");
    Serial.print(MAX_SAMPLE_RATE);
    Serial.println(" Hz");
    Serial.print("   Average adapted rate: ");
    Serial.print(getCurrentSampleRate());
    Serial.println(" Hz");
    Serial.print("   Average energy saved: ");
    Serial.print(avg_energy_saved);
    Serial.println("%\n");
    
    // Data Volume
    Serial.print("2. DATA VOLUME\n");
    Serial.print("   Raw data if oversampled: ");
    Serial.print(total_raw_samples_if_oversampled * 4);
    Serial.println(" bytes");
    Serial.print("   MQTT data sent (adaptive): ");
    Serial.print(total_mqtt_bytes_sent);
    Serial.println(" bytes");
    
    float data_reduction = (1.0 - (float)total_mqtt_bytes_sent / (total_raw_samples_if_oversampled * 4)) * 100;
    Serial.print("   Data reduction: ");
    Serial.print(data_reduction);
    Serial.println("%\n");
    
    // Latency
    float avg_latency = total_latency_ms / latency_samples;
    Serial.print("3. END-TO-END LATENCY\n");
    Serial.print("   Average: ");
    Serial.print(avg_latency);
    Serial.println(" ms");
    Serial.print("   Minimum: ");
    Serial.print(min_latency);
    Serial.println(" ms");
    Serial.print("   Maximum: ");
    Serial.print(max_latency);
    Serial.println(" ms\n");
    
    // Execution Time
    float avg_fft_time = (float)total_fft_time_us / total_windows_processed;
    Serial.print("4. PER-WINDOW EXECUTION TIME\n");
    Serial.print("   Average FFT computation: ");
    Serial.print(avg_fft_time);
    Serial.println(" us");
    Serial.print("   Windows processed: ");
    Serial.println(total_windows_processed);
    
    Serial.println("==========================================");
    Serial.println("        END OF REPORT");
    Serial.println("==========================================\n");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
