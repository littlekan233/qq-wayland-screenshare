
#include <string>
#include <thread>
#include <tuple>

#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>

// got to include this before X11 headers
#include "hook_opencv.hpp"

#include <X11/Xlib.h>

#include "framebuf.hpp"
#include "payload.hpp"
#include "interface.hpp"
#include "helpers.hpp"
#include "hook.hpp"

/*

  Why I'm using STB here:
  Initially I tried to use opencv as I actually have worked with it before
  However for *some* reason long as the opencv is linked to the hook
  the hook would always crash wemeetapp. I suspect that maybe the wemeetapp
  itself is also using opencv and thereby some conflict arises.

  I also tried the CImg library but its in-memory image layout is just ABSURD,
  which could cause severe performance issue.

  So I eventually picked STB, and I'm happy with it now.

*/

// #define STB_IMAGE_RESIZE_IMPLEMENTATION
// #include <stb/stb_image_resize2.h>

// Forward declaration
void x11_sanitizer_main();

constexpr uint32_t DEFAULT_FRAME_HEIGHT = 1080;
constexpr uint32_t DEFAULT_FRAME_WIDTH = 1920;

namespace {
  std::atomic<bool> payload_initialized{false};
  std::atomic<bool> payload_init_requested{false};
  std::atomic<int> screenshare_detection_score{0};
}

// Lazy initialization: only start payload when we detect actual screenshare activity
static void try_init_payload() {
  if (payload_initialized.load(std::memory_order_seq_cst)) {
    return;
  }

  bool expected = false;
  if (!payload_init_requested.compare_exchange_strong(expected, true, std::memory_order_seq_cst)) {
    return; // Already being initialized
  }

  fprintf(stderr, "%s", green_text("[hook] starting payload initialization\n").c_str());

  auto& interface_singleton = InterfaceSingleton::getSingleton();

  // Start x11_sanitizer early to hide QQ's screenshare window ASAP
  std::thread x11_sanitizer_thread = std::thread(x11_sanitizer_main);
  x11_sanitizer_thread.detach();
  fprintf(stderr, "%s", green_text("[hook] x11_sanitizer thread started early\n").c_str());

  // initialize interface singleton:
  interface_singleton.interface_handle = new Interface(
    DEFAULT_FB_ALLOC_HEIGHT, DEFAULT_FB_ALLOC_WIDTH,
    DEFAULT_FRAME_HEIGHT, DEFAULT_FRAME_WIDTH, SpaVideoFormat_e::RGBA
  );
  interface_singleton.portal_handle = new XdpScreencastPortal();

  // start the payload thread - it will handle the rest asynchronously
  std::thread payload_thread = std::thread([]() {
    auto& interface_singleton = InterfaceSingleton::getSingleton();

    payload_main();

    // Wait for pipewire to be ready
    while(interface_singleton.pipewire_handle.load() == nullptr){
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    payload_initialized.store(true, std::memory_order_seq_cst);
    fprintf(stderr, "%s", green_text("[hook] payload fully initialized\n").c_str());
  });
  payload_thread.detach();

  fprintf(stderr, "%s", green_text("[hook] payload thread started\n").c_str());
}

void XShmAttachHook(){
  // Don't initialize payload here - wait for actual screenshare activity
  fprintf(stderr, "%s", yellow_text("[hook] XShmAttach called, waiting for screenshare activity...\n").c_str());
}

template <typename T>
struct remove_pointer_cvref {
  using type = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<T>>>;
};

template <typename T>
using remove_pointer_cvref_t = typename remove_pointer_cvref<T>::type;


// returns: ximage_width_offset, ximage_height_offset, target_width, target_height
std::tuple<uint32_t, uint32_t, uint32_t, uint32_t> get_resize_param(
  uint32_t ximage_width,
  uint32_t ximage_height,
  uint32_t framebuffer_width,
  uint32_t framebuffer_height
){
  // keep the framebuffer aspect ratio
  double framebuffer_aspect_ratio = static_cast<double>(framebuffer_width) / static_cast<double>(framebuffer_height);
  double ximage_aspect_ratio = static_cast<double>(ximage_width) / static_cast<double>(ximage_height);

  uint32_t target_width = 0;
  uint32_t target_height = 0;
  uint32_t ximage_width_offset = 0;
  uint32_t ximage_height_offset = 0;

  if (framebuffer_aspect_ratio > ximage_aspect_ratio) {
    // framebuffer is wider than ximage
    target_width = ximage_width;
    target_height = (ximage_width * framebuffer_height) / framebuffer_width;
    ximage_height_offset = (ximage_height - target_height) / 2;
  } else {
    // framebuffer is taller than ximage
    target_height = ximage_height;
    target_width = ximage_height * framebuffer_width / framebuffer_height;
    ximage_width_offset = (ximage_width - target_width) / 2;
  }

  return std::make_tuple(ximage_width_offset, ximage_height_offset, target_width, target_height);
}


void XShmGetImageHook(XImage& image){

  auto& interface_singleton = InterfaceSingleton::getSingleton();

  if (interface_singleton.interface_handle.load() == nullptr){
    fprintf(stderr, "%s", red_text("[hook] hook will NOT work as you have cancelled the screencast!!!\n").c_str());
    return;
  }

  auto ximage_spa_format = ximage_to_spa(image);
  auto ximage_width = image.width;
  auto ximage_height = image.height;
  size_t ximage_bytes_per_line = image.bytes_per_line;

  CvMat ximage_cvmat;
  OpencvDLFCNSingleton::cvInitMatHeader(
    &ximage_cvmat, ximage_height, ximage_width,
    CV_8UC4, image.data, ximage_bytes_per_line
  );
  OpencvDLFCNSingleton::cvSetZero(&ximage_cvmat);

  auto& framebuffer = interface_singleton.interface_handle.load()->framebuf;
  auto framebuffer_spa_format = framebuffer.format;
  auto framebuffer_width = framebuffer.width;
  auto framebuffer_height = framebuffer.height;
  auto framebuffer_row_byte_stride = framebuffer.row_byte_stride;

  CvMat framebuffer_cvmat;
  OpencvDLFCNSingleton::cvInitMatHeader(
    &framebuffer_cvmat, framebuffer_height, framebuffer_width,
    CV_8UC4, framebuffer.data.get(), framebuffer_row_byte_stride
  );
  CvMat *framebuffer_cvmat_ptr = &framebuffer_cvmat;
  if (framebuffer.crop_height && framebuffer.crop_width) {
    OpencvDLFCNSingleton::cvGetSubRect(framebuffer_cvmat_ptr, framebuffer_cvmat_ptr, {framebuffer.crop_x, framebuffer.crop_y, framebuffer.crop_width, framebuffer.crop_height});
    framebuffer_width = framebuffer.crop_width;
    framebuffer_height = framebuffer.crop_height;
  }
  if (framebuffer.rotate) {
    if (framebuffer.rotate != 180) {
      std::swap(framebuffer_width, framebuffer_height);
    }
    CvMat *framebuffer_cvmat_rotated = OpencvDLFCNSingleton::cvCreateMat(framebuffer_height, framebuffer_width, CV_8UC4);
    OpencvDLFCNSingleton::cvRotate(framebuffer_cvmat_ptr, framebuffer_cvmat_rotated, framebuffer.rotate);
    framebuffer_cvmat_ptr = framebuffer_cvmat_rotated;
  }
  if (framebuffer.flip) {
    OpencvDLFCNSingleton::cvFlip(framebuffer_cvmat_ptr);
  }

  
  // get the resize parameters
  auto [ximage_width_offset, ximage_height_offset, target_width, target_height] = get_resize_param(
    ximage_width, ximage_height, framebuffer_width, framebuffer_height
  );
  CvMat ximage_cvmat_roi;
  OpencvDLFCNSingleton::cvGetSubRect(
    &ximage_cvmat, &ximage_cvmat_roi,
    cvRect(ximage_width_offset, ximage_height_offset, target_width, target_height)
  );
  OpencvDLFCNSingleton::cvResize(
    framebuffer_cvmat_ptr, &ximage_cvmat_roi, CV_INTER_LINEAR
  );
  
  if (framebuffer_cvmat_ptr != &framebuffer_cvmat) {
    OpencvDLFCNSingleton::cvReleaseMat(&framebuffer_cvmat_ptr);
  }

  // do color convert
  // here the code is currently mainly for wlroot WMs
  // maybe we could shortcut this by detecting WM?

  int cv_cAPI_color_cvt_code = get_opencv_cAPI_color_convert_code(
    framebuffer_spa_format, ximage_spa_format
  );

  if (cv_cAPI_color_cvt_code != -1){
    // non -1 code means color conversion is needed
    OpencvDLFCNSingleton::cvCvtColor(
      &ximage_cvmat_roi, &ximage_cvmat_roi, cv_cAPI_color_cvt_code
    );
  }

  // legacy stb implementation
  // resize the framebuffer to ximage size
  // note: by using STBIR_BGRA_PM we are essentially ignoring the alpha channel
  // heck, I don't even know if the alpha channel is used in the first place
  // Anyway, we are just going to ignore it for now since this will be much faster
  // stbir_resize_uint8_srgb(
  //   reinterpret_cast<uint8_t*>(framebuffer.data.get()),
  //   framebuffer_width, framebuffer_height, framebuffer_row_byte_stride,
  //   reinterpret_cast<uint8_t*>(image.data),
  //   ximage_width, ximage_height, ximage_bytes_per_line,
  //   stbir_pixel_layout::STBIR_BGRA_PM
  // );


  return;
  
}



void XShmDetachStopPWLoop(){
  auto& interface_singleton = InterfaceSingleton::getSingleton();
  fprintf(stderr, "%s", green_text("[hook] signal pw stop.\n").c_str());
  interface_singleton.interface_handle.load()->pw_stop_flag.store(true, std::memory_order_seq_cst);
  while(!interface_singleton.interface_handle.load()->payload_pw_stop_confirm.load(std::memory_order_seq_cst)){
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  fprintf(stderr, "%s", green_text("[hook SYNC] pw stop confirmed.\n").c_str());
  return;
}

void XShmDetachStopGIOLoop(){
  auto& interface_singleton = InterfaceSingleton::getSingleton();
  fprintf(stderr, "%s", green_text("[hook] stop gio main loop.\n").c_str());
  g_main_loop_quit(interface_singleton.portal_handle.load()->gio_mainloop);
  while(!interface_singleton.interface_handle.load()->payload_gio_stop_confirm.load(std::memory_order_seq_cst)){
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  fprintf(stderr, "%s", green_text("[hook SYNC] gio stop confirmed.\n").c_str());
  return;
}

void XShmDetachHook(){

  auto& interface_singleton = InterfaceSingleton::getSingleton();
  
  // the interface_handle being non nullptr
  // means that the screencast has been started
  if (interface_singleton.interface_handle != nullptr){

    XShmDetachStopPWLoop();
    XShmDetachStopGIOLoop();

    // normal de-initialize interface singleton:
    // (1) free the interface object
    delete interface_singleton.interface_handle.load();
    interface_singleton.interface_handle.store(nullptr);
    // (2) free the screencast portal object
    delete interface_singleton.portal_handle.load();
    interface_singleton.portal_handle.store(nullptr);
    // (3) free the pipewire screencast object
    delete interface_singleton.pipewire_handle.load();
    interface_singleton.pipewire_handle.store(nullptr);
  } else {
    // we do nothing here since the objects (interface, portal) are already freed,
    // and pipewire object has never been created
    fprintf(stderr, "%s", red_text("[hook] objects are already freed because of cancelled screencast. exiting.\n").c_str());
  }
  

}

extern "C" {

Bool XShmAttach(Display* dpy, XShmSegmentInfo* shminfo){
  XShmAttachHook();
  return XShmAttachFunc(dpy, shminfo);
}

Bool XShmGetImage(Display* dpy, Drawable d, XImage* image, int x, int y, unsigned long plane_mask){
  // Detect screenshare activity: large, frequent image requests
  // Only detect if not already initialized or initializing
  if (!payload_initialized.load(std::memory_order_seq_cst) &&
      !payload_init_requested.load(std::memory_order_seq_cst)) {
    bool is_large_image = (image->width >= 1280 && image->height >= 720);
    bool is_root_window = (d == DefaultRootWindow(dpy));

    if (is_large_image || is_root_window) {
      int score = screenshare_detection_score.fetch_add(1, std::memory_order_seq_cst) + 1;
      if (score >= 5) {
        fprintf(stderr, "%s", green_text("[hook] screenshare activity detected, initializing payload\n").c_str());
        try_init_payload();
      }
    }
  }

  // Only apply hook if payload is initialized
  if (payload_initialized.load(std::memory_order_seq_cst)) {
    XShmGetImageHook(*image);
  }
  return 1;
}

Bool XShmDetach(Display* dpy, XShmSegmentInfo* shminfo){
  // Clean up and reset state for next screenshare session
  if (payload_initialized.load(std::memory_order_seq_cst)) {
    fprintf(stderr, "%s", yellow_text("[hook] XShmDetach called, cleaning up resources\n").c_str());
    XShmDetachHook();

    // Reset all state flags so next screenshare can initialize properly
    payload_initialized.store(false, std::memory_order_seq_cst);
    payload_init_requested.store(false, std::memory_order_seq_cst);
    screenshare_detection_score.store(0, std::memory_order_seq_cst);

    fprintf(stderr, "%s", green_text("[hook] cleanup complete, ready for next screenshare session\n").c_str());
  }

  return XShmDetachFunc(dpy, shminfo);
}

Bool XDamageQueryExtension(Display *dpy, int *event_base_return, int *error_base_return) {
  return 0;
}

}
