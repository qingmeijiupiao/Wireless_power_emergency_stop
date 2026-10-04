#pragma once
namespace BlackboxService{struct Statistics{unsigned pending_logs=0,captured_logs=0,dropped_logs=0,persist_failures=0;};
inline void get_statistics(Statistics*){}}
