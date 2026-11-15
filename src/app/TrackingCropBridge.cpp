#include "app/TrackingCropBridge.h"
#include "app/AppPaths.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <algorithm>
namespace app {
namespace fs = std::filesystem;
TrackingCropBridge::~TrackingCropBridge() {
    stopped_=true;
    if(watcher_.joinable())watcher_.join();
    pipe_.kill();
    if(!options_.empty()){std::error_code ec;fs::remove_all(options_,ec);}
}
std::string TrackingCropBridge::renderer_path(const std::string& configured) {
    if(!configured.empty())return configured;
    if(const char* env=std::getenv("GOPRO_PLAYER_RENDERER"))return env;
    return (fs::path(exe_dir())/"sphere-export-render").string();
}
bool TrackingCropBridge::start(const std::string& renderer,const std::string& cache_folder,
 const std::string& video,const std::vector<int64_t>& frames,const std::atomic<bool>& cancel,
 std::function<void(const std::string&)> log,std::string& error) {
    std::error_code ec;
    auto executable=fs::canonical(renderer_path(renderer),ec);
    if(ec){error="GoPro renderer not found: "+renderer_path(renderer);return false;}
    const auto name=fs::path(video).filename().string()+".people.bin";
    fs::path cache=cache_folder.empty()?fs::path(video).parent_path()/name:fs::path(cache_folder)/name;
    if(cache_folder.empty()&&!fs::exists(cache))cache=executable.parent_path().parent_path()/"dataset/outlines"/name;
    if(!fs::exists(cache)){error="Tracking file not found: "+cache.string();return false;}
    if(frames.empty()){error="No source frames for GoPro bridge";return false;}
    for(const auto& p:{video,cache.string()})if(p.find_first_of("\r\n")!=std::string::npos){error="Invalid path for GoPro renderer";return false;}
    for(int i=0;i<100;i++){
        auto key=std::chrono::steady_clock::now().time_since_epoch().count();
        auto dir=fs::temp_directory_path()/ ("spirula-gopro-"+std::to_string(key)+"-"+std::to_string(i));
        if(fs::create_directory(dir,ec)){options_=dir.string();break;}
    }
    if(options_.empty()){error="Cannot create GoPro bridge options";return false;}
    auto ini=fs::path(options_)/"request.ini";
    auto escape=[](const std::string& value){std::string out;for(char c:value){if(c=='\\')out+='\\';out+=c;}return out;};
    std::ofstream f(ini);
    f<<"[export]\ninput="<<escape(video)<<"\ntracking_cache="<<escape(cache.string())
     <<"\ntracking_crops=true\nwidth=1008\nheight=1008\nmode=2\nhardware=1\nprojection=0\n"
       "fps=1\nstart=0\nend=1000000000\nyaw=0\npitch=0\nfov=90\nsharpness=0\nselected_frames=";
    int64_t last=-1;
    for(auto n:frames){if(n<0||n<=last){error="GoPro requests must be ordered source-frame indices";return false;}f<<n<<';';last=n;}
    f<<'\n';f.close();if(!f){error="Cannot write GoPro bridge options";return false;}
    log("GoPro tracking: "+cache.string());
    if(!pipe_.start({executable.string(),ini.string()},log)){error="Cannot start GoPro renderer";return false;}
    watcher_=std::thread([this,&cancel]{while(!stopped_){if(cancel.load()){pipe_.kill();break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(50));}});
    return true;
}
bool TrackingCropBridge::exact(void* dst,size_t bytes){
    auto p=static_cast<uint8_t*>(dst);while(bytes){size_t n=pipe_.read(p,bytes);if(!n)return false;p+=n;bytes-=n;}return true;
}
bool TrackingCropBridge::line(std::string& out){
    out.clear();char c;while(out.size()<2048&&exact(&c,1)){if(c=='\n')return true;out+=c;}return false;
}
bool TrackingCropBridge::read(int64_t index,TrackingFrame& frame,std::string& error){
    std::string text,tag;int n=0,fallback=0;frame=TrackingFrame{};
    if(!line(text)){
        // The process's exit status is the one fact a log with no renderer
        // output leaves out: 127 names an exec-level failure, 139 a crash,
        // 137 SIGKILL, 0 a clean exit before it answered.
        const int status=pipe_.finish();
        error="GoPro crop stream ended before frame "+std::to_string(index)+
              " (renderer exit "+std::to_string(status)+"); see renderer log";
        return false;
    }
    std::istringstream header(text);header>>tag>>frame.index>>frame.time>>n>>fallback;
    if(!header||tag!="FRAME"||frame.index!=index||!std::isfinite(frame.time)||n<1||n>64||(fallback!=0&&fallback!=1)){
        error="Invalid GoPro frame header: "+text;return false;}
    frame.fallback=fallback!=0;
    for(int i=0;i<n;i++){
        TrackingCrop c;
        if(!line(text)){error="Missing GoPro crop header";return false;}
        std::istringstream row(text);row>>tag>>c.image.width>>c.image.height>>c.tangent;
        for(double& v:c.rotation)row>>v;
        if(!row||tag!="CROP"||c.image.width!=1008||c.image.height!=1008||!std::isfinite(c.tangent)||c.tangent<=0||c.tangent>4){error="Invalid GoPro crop geometry";return false;}
        for(double v:c.rotation)if(!std::isfinite(v)){error="Nonfinite GoPro rotation";return false;}
        c.image.channels=3;c.image.data.resize((size_t)c.image.width*c.image.height*3);
        if(!exact(c.image.data.data(),c.image.data.size())){error="Truncated GoPro crop";return false;}
        frame.crops.push_back(std::move(c));
    }
    return true;
}
bool TrackingCropBridge::finish(std::string& error){
    stopped_=true;if(watcher_.joinable())watcher_.join();int status=pipe_.finish();
    if(status){error="GoPro renderer failed ("+std::to_string(status)+")";return false;}return true;
}
std::vector<float> tracking_target_rays(const Pano360Layout& l,Pano360View view,int w,int h){
    view.width=w;view.height=h;Pano360Remap map;pano360_remap(l,view,map);
    std::vector<float> rays((size_t)w*h*3);
    // Match existing Spirula pixels through their source coordinates, including its EAC seam mapping.
    for(size_t i=0;i<map.x.size();i++){
        // Outside the lens (a fisheye's corners, marked -1): zero ray, which
        // project_tracking_masks skips (z<=0), so the black corner keeps.
        if(!(map.x[i]>=0.0f&&map.y[i]>=0.0f))continue;
        double x=map.x[i]+.5,y=map.y[i]+.5;int row=y>=l.track_h;y-=row*l.track_h;
        double raw=x;if(x>=l.face/2.)raw+=l.strip;if(x>=2.5*l.face)raw+=l.strip;
        double stored=(l.track_w-l.track_h)/2.,span=stored-64,u,v;
        int cell;
        if(raw>=stored&&raw<stored+l.track_h){
            if(!row){cell=1;u=(raw-stored)/l.track_h;v=y/l.track_h;}
            else{cell=4;u=y/l.track_h;v=1-(raw-stored)/l.track_h;}
        }else{
            int right=raw>=stored+l.track_h;double local=raw-(right?l.track_w-stored:0);
            double sx=(local-(local>=stored*.5?64:0))/span,sy=y/l.track_h;
            if(!row){cell=right?2:0;u=sx;v=sy;}
            else{cell=right?5:3;u=1-sy;v=sx;}
        }
        u=std::tan((u-.5)*1.5707963267948966);v=std::tan((v-.5)*1.5707963267948966);
        double a=0,b=0,c=0;
        switch(cell){case 0:a=-1;b=-v;c=u;break;case 1:a=u;b=-v;c=1;break;
         case 2:a=1;b=-v;c=-u;break;case 3:a=u;b=-1;c=-v;break;
         case 4:a=-u;b=-v;c=-1;break;default:a=u;b=1;c=v;break;}
        double norm=std::sqrt(a*a+b*b+c*c);rays[i*3]=a/norm;rays[i*3+1]=-b/norm;rays[i*3+2]=c/norm;
    }
    return rays;
}
void project_tracking_masks(const std::vector<float>& rays,const TrackingFrame& frame,std::vector<uint8_t>& mask){
    mask.assign(rays.size()/3,255);
    for(const auto& c:frame.crops){
        if(c.mask.size()!=(size_t)c.image.width*c.image.height)continue;
        for(size_t i=0;i<mask.size();i++){
            if(!mask[i])continue;
            const float* r=&rays[i*3];const double* m=c.rotation;
            double z=r[0]*m[2]+r[1]*m[5]+r[2]*m[8];if(z<=0)continue;
            double x=(r[0]*m[0]+r[1]*m[3]+r[2]*m[6])/(z*c.tangent);
            double y=(r[0]*m[1]+r[1]*m[4]+r[2]*m[7])/(z*c.tangent);
            if(std::abs(x)>=1||std::abs(y)>=1)continue;
            int px=std::clamp(int((x+1)*.5*c.image.width),0,c.image.width-1);
            int py=std::clamp(int((y+1)*.5*c.image.height),0,c.image.height-1);
            if(c.mask[(size_t)py*c.image.width+px]<128)mask[i]=0;
        }
    }
}
}
