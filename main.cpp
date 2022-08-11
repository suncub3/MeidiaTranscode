#include "transcode.h"

int main() {
  std::string input_url = "../res/fps25.mp4";
  std::string output_url = "../res/res.mp4";

  std::shared_ptr<Transcode> transcode = std::make_shared<Transcode>();
  LOG(INFO) << "start";
  transcode->doTranscode(input_url, output_url);
  LOG(INFO) << "finish";
  return 0;
}
