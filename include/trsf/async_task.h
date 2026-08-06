#pragma once

#include <functional>
#include <utility>

namespace qcloud_cos {

using TaskFunc = std::function<void()>;

class AsyncTask {
 public:
  explicit AsyncTask(TaskFunc &&f) : _f(std::move(f)) {}
  ~AsyncTask() {}

  void runTask() { _f(); }

 private:
  TaskFunc _f;
};

}  // namespace qcloud_cos
