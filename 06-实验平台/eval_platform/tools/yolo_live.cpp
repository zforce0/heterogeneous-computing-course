#include "yolov8.h"
#include "postprocess.h"
#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <limits.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>

// 可见光摄像头 USB 标识（RV1126B 板载 UVC 模组）
static const unsigned VIS_VID = 0x1D6C;
static const unsigned VIS_PID = 0x0103;

static long long now_ms(){ struct timeval t; gettimeofday(&t,0); return (long long)t.tv_sec*1000+t.tv_usec/1000; }

static void write_atomic(const char* tmp,const char* dst,const std::string& data){
  FILE* f=fopen(tmp,"wb"); if(!f) return;
  fwrite(data.data(),1,data.size(),f); fclose(f);
  rename(tmp,dst);
}

static std::string read_line(const std::string& path){
  std::ifstream f(path.c_str());
  std::string s;
  if(!f) return std::string();
  std::getline(f, s);
  while(!s.empty() && (s[s.size()-1]=='\n' || s[s.size()-1]=='\r')) s.erase(s.size()-1);
  return s;
}

// 从 /sys/class/video4linux/videoN/device 解析出的真实路径向上找 idVendor/idProduct
static bool path_has_usb_id(const std::string& start, unsigned vid, unsigned pid){
  std::string cur = start;
  for (int i = 0; i < 6; ++i) {
    std::string v = read_line(cur + "/idVendor");
    std::string p = read_line(cur + "/idProduct");
    if (!v.empty() && !p.empty()) {
      return strtoul(v.c_str(), 0, 16) == vid && strtoul(p.c_str(), 0, 16) == pid;
    }
    size_t slash = cur.rfind('/');
    if (slash == std::string::npos || slash == 0) break;
    cur = cur.substr(0, slash);
  }
  return false;
}

static void push_unique(std::vector<std::string>& v, const std::string& s){
  if (s.empty()) return;
  if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
}

// 候选设备：环境变量 > /dev/g4-vis 别名 > 按 VID:PID 匹配 > 其余 capture 节点
// 换 USB 口后 /dev/videoN 编号会变，因此绝不写死路径。
static std::vector<std::string> camera_candidates(std::vector<std::string>& pref_out){
  std::vector<std::string> pref, fallback;

  const char* env = getenv("G4_VIS_DEV");
  if (env && *env) push_unique(pref, env);
  if (access("/dev/g4-vis", F_OK) == 0) push_unique(pref, "/dev/g4-vis");

  DIR* dir = opendir("/sys/class/video4linux");
  if (dir) {
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
      if (strncmp(ent->d_name, "video", 5) != 0) continue;
      std::string dev = std::string("/dev/") + ent->d_name;
      push_unique(fallback, dev);
      std::string link = std::string("/sys/class/video4linux/") + ent->d_name + "/device";
      char rp[PATH_MAX];
      if (realpath(link.c_str(), rp) == NULL) continue;
      if (path_has_usb_id(rp, VIS_VID, VIS_PID)) push_unique(pref, dev);
    }
    closedir(dir);
  }
  std::sort(pref.begin(), pref.end());
  std::sort(fallback.begin(), fallback.end());
  pref_out = pref;

  std::vector<std::string> all = pref;
  for (size_t i = 0; i < fallback.size(); ++i) push_unique(all, fallback[i]);
  return all;
}

// 打开并预读一帧，确认这个节点真的能出图（避免选到 metadata 节点）
static bool open_cam(cv::VideoCapture& cap, const std::string& dev){
  cap.release();
  if (!cap.open(dev, cv::CAP_V4L2)) return false;
  cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
  cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
  cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
  cv::Mat probe;
  if (!cap.read(probe) || probe.empty()) { cap.release(); return false; }
  return true;
}

// 逐个候选尝试，成功则返回实际使用的设备路径
static std::string acquire(cv::VideoCapture& cap){
  std::vector<std::string> pref;
  std::vector<std::string> cands = camera_candidates(pref);
  for (size_t i = 0; i < cands.size(); ++i) {
    if (open_cam(cap, cands[i])) {
      fprintf(stderr, "[yolo_live] camera = %s%s\n", cands[i].c_str(),
              (std::find(pref.begin(), pref.end(), cands[i]) != pref.end()) ? "  (VID:PID match)" : "  (fallback)");
      return cands[i];
    }
  }
  return std::string();
}

static std::string base_name(const std::string& p){
  size_t s = p.rfind('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

int main(int argc, char** argv){
  // 模型路径可换（argv[1] > 环境变量 YOLO_MODEL > 默认），方便现场换模型对比；
  // 具体用哪个模型不靠猜：跑起来后把文件名/路径/字节数写进 live_boxes.json，
  // 由 status.json 透出，页面自己显示，换模型重启服务即可自动更新。
  std::string model = "/userdata/rv1126_test/yolov8m_rv1126b.rknn";
  if (const char* env = getenv("YOLO_MODEL")) if (*env) model = env;
  if (argc > 1 && argv[1][0] != '-') model = argv[1];

  cv::VideoCapture cap;
  std::string dev;
  int spins = 0;
  while (dev.empty()) {
    dev = acquire(cap);
    if (dev.empty()) {
      // 开机时摄像头可能还没枚举完；在本进程内等，不要靠 systemd 反复重启
      if ((spins++ % 10) == 0) fprintf(stderr, "[yolo_live] waiting for a usable camera...\n");
      sleep(2);
    }
  }
  if (!cap.isOpened()) {
    fprintf(stderr, "[yolo_live] open camera failed: no usable V4L2 capture node\n");
    return 1;
  }

  rknn_app_context_t ctx; memset(&ctx,0,sizeof(ctx));
  if(yolov8_detect_init(model.c_str(),&ctx)<0){ fprintf(stderr,"[yolo_live] rknn init failed: %s\n", model.c_str()); return 2; }

  long long model_bytes = 0; long long model_mtime = 0;
  {
    struct stat st;
    if (stat(model.c_str(), &st) == 0) {
      model_bytes = (long long)st.st_size;
      model_mtime = (long long)st.st_mtim.tv_sec;
    }
  }
  fprintf(stderr,"[yolo_live] started model=%s (%lld bytes)\n", model.c_str(), model_bytes);
  long long seq=0;
  int fails=0;
  for(;;){
    cv::Mat img;
    if(!cap.read(img) || img.empty()){
      // 掉线（换口/线松/设备重枚举）→ 重新解析候选并重开，而不是一直空转
      if(++fails >= 30){
        fprintf(stderr,"[yolo_live] camera lost (%d fails), re-resolving\n", fails);
        cap.release();
        dev = acquire(cap);
        if(dev.empty()) fprintf(stderr,"[yolo_live] no camera yet, will retry\n");
        fails = 0;
      }
      usleep(100000);
      continue;
    }
    fails=0;
    ++seq;
    detect_result_group_t od; memset(&od,0,sizeof(od));
    double infer_ms=0;
    yolov8_detect_run(&ctx,img,&od,&infer_ms);
    std::vector<uchar> jpg;
    cv::imencode(".jpg",img,jpg);
    write_atomic("/tmp/live_frame.tmp","/tmp/live_frame.jpg",std::string((char*)jpg.data(),jpg.size()));
    char b[256];
    std::string js="{\"ts_ms\":"+std::to_string(now_ms())+",\"seq\":"+std::to_string(seq)+
      ",\"infer_ms\":"+std::to_string((int)infer_ms)+",\"res\":["+
      std::to_string(img.cols)+","+std::to_string(img.rows)+"],\"device\":\""+dev+"\","+
      "\"model\":\""+base_name(model)+"\","+
      "\"model_path\":\""+model+"\","+
      "\"model_bytes\":"+std::to_string(model_bytes)+","+
      "\"model_mtime\":"+std::to_string(model_mtime)+",\"detections\":[";
    for(int i=0;i<od.count && i<20;i++){
      const detect_result_t& d=od.results[i];
      if(i) js+=",";
      snprintf(b,sizeof(b),"{\"name\":\"%s\",\"score\":%.3f,\"box\":[%d,%d,%d,%d]}",
        d.name,d.prop,d.box.left,d.box.top,d.box.right,d.box.bottom);
      js+=b;
    }
    js+="]}";
    write_atomic("/tmp/live_boxes.tmp","/tmp/live_boxes.json",js);
    usleep(30000);
  }
  yolov8_detect_release(&ctx);
  return 0;
}
