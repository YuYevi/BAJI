#ifndef ABNORMAL_REPORTER_H
#define ABNORMAL_REPORTER_H

namespace AbnormalReporter {

void Initialize();
void MarkExpectedReset(const char* reason);
void QueueEvent(const char* type, const char* json);
void PublishPendingEvents();

}  // namespace AbnormalReporter

#endif
