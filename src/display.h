#ifndef DISPLAY_H
#define DISPLAY_H


void display_init();
void display_update(unsigned long currentMillis);
// Shows or removes the firmware update screen to match otaIsRunning() and draws it
// immediately. For the web upload, which blocks the loop task for the whole upload.
void display_refresh_ota_screen();
void displayStatusMessages_t(void* pvParameters);

#endif // DISPLAY_H
