#pragma once
#include "esp_err.h"

esp_err_t mqtt_pub_init(void);
void      mqtt_pub_reconfigure(void);
void      mqtt_pub_stop(void);
void      mqtt_pub_task(void *arg);
bool      mqtt_pub_is_connected(void);

/* Power sensor discovery for ups.realpower override */
#define MAX_POWER_SENSORS 24
typedef struct {
    char name[64];       /* display name from HA discovery */
    char topic[128];     /* MQTT state_topic */
    char json_key[32];   /* JSON key to extract value, empty = raw */
    char unique_id[64];  /* HA unique_id */
} power_sensor_t;

int                    mqtt_get_power_sensors(const power_sensor_t **out);
void                   mqtt_subscribe_power_topic(const char *topic, const char *json_key);
