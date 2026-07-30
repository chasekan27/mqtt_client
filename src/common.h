/*
 * Copyright (c) 2020 Prevas A/S
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define WIFI_FILE_PATH "/lfs1/wifi_cred.txt"
#define AES_BLOCK_SIZE  16

typedef struct wifi_credentials
{
    uint8_t ssid[32];
    uint8_t password[64];
}wifi_credentials_t;

void init_wifi(void);
bool check_wifi_status();
int start_mqtt_client();
int update_wifi_cred();
void wifi_disconnect(void);
