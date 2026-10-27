#ifndef LOGGING_H
#define LOGGING_H

void logAndPublish(const char* messageBuffer);
void errorPublish(const char* messageBuffer);
// Logs the calling task's stack high-water mark once an hour (and on the first call).
// lastLogMs is the caller's own millis() timestamp of its previous log.
void logStackHwmIfDue(const char* taskName, unsigned long* lastLogMs);

#endif // LOGGING_H
