#ifndef MQTT_H
#define MQTT_H


// Creates the publish queue and configures the client. Call once in setup() before
// any logging and before the receive task starts.
void mqtt_init();
// FreeRTOS task that owns the MQTT client: connects, receives and publishes.
void receive_mqtt_messages_t(void* pvParameters);
// True while the receive task has a live broker connection (safe from any task).
bool mqttIsConnected();
// Queues a message for the receive task to publish to the log topic, or the retained
// error topic when isError. Never blocks; returns false if the queue is full.
bool mqttQueuePublish(const char* message, bool isError);

#endif // MQTT_H
