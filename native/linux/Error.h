#pragma once
#include <string>
namespace meeting_record::linux_backend {
inline thread_local std::string last_error;
inline thread_local int last_status = 0;
inline int Fail(int status, const std::string &detail) {
  last_error = detail;
  last_status = status;
  return status;
}
} // namespace meeting_record::linux_backend
