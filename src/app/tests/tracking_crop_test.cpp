#include "app/TrackingCropBridge.h"
#include "sam/Masking.h"
#include <iostream>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <stdexcept>
static void require(bool ok,const std::string& text){if(!ok)throw std::runtime_error(text);}
int main(int argc,char** argv){try{
    app::TrackingFrame f;app::TrackingCrop c;c.image.width=4;c.image.height=4;c.tangent=1;
    c.rotation[0]=c.rotation[4]=c.rotation[8]=1;c.mask.assign(16,255);c.mask[10]=0;f.crops.push_back(c);
    std::vector<uint8_t> mask;app::project_tracking_masks({.25f,.25f,1, -.75f,-.75f,1, 0,0,-1, 2,0,1},f,mask);
    require(mask==std::vector<uint8_t>({0,255,255,255}),"Mask polarity/frustum test failed");
    app::Pano360Layout layout;require(app::pano360_detect(2,4096,1344,{},layout),"MAX layout");
    for(auto mode:{app::Pano360Mode::Faces,app::Pano360Mode::Corners}){
        app::Pano360Options opt;opt.mode=mode;opt.size=128;
        for(auto v:app::pano360_views(layout,opt)){
            auto rays=app::tracking_target_rays(layout,v,v.width,v.height);
            for(size_t i=0;i<rays.size();i+=3)require(std::abs(rays[i]*rays[i]+rays[i+1]*rays[i+1]+rays[i+2]*rays[i+2]-1)<1e-5,"Non-unit ray");
        }
    }
    {
        // A fisheye's corners are outside the lens: no ray, and the zero must
        // keep the pixel, which a resampled frame has left black.
        app::Pano360Options opt;opt.mode=app::Pano360Mode::Fisheye;opt.size=128;
        int zero=0,unit=0;
        for(auto v:app::pano360_views(layout,opt)){
            auto rays=app::tracking_target_rays(layout,v,v.width,v.height);
            app::project_tracking_masks(rays,f,mask);
            for(size_t i=0;i<rays.size();i+=3){
                const float n=rays[i]*rays[i]+rays[i+1]*rays[i+1]+rays[i+2]*rays[i+2];
                if(n==0){require(mask[i/3]==255,"A zero ray must keep its pixel");++zero;continue;}
                require(std::abs(n-1)<1e-5,"Non-unit ray");
                ++unit;
            }
        }
        require(zero>0&&unit>0,"A fisheye's corners are zero rays and its lens unit ones");
    }
    std::cout<<"Projection unit checks passed\n";
    if(argc!=6)return 0;
    std::filesystem::create_directories(argv[5]);std::atomic<bool> cancel{false};std::string error;
    app::TrackingCropBridge bridge;
    require(bridge.start(argv[1],argv[2],argv[3],{1139},cancel,[](const std::string& t){std::cerr<<t<<'\n';},error),error);
    require(bridge.read(1139,f,error),error);require(bridge.finish(error),error);
    require(!f.fallback&&!f.crops.empty(),"Expected tracked crops");
    sam::MaskOptions mo;mo.model=argv[4];mo.text="person; human shadow";mo.feature_text="sky";mo.video=false;
    sam::Masker masker;require(masker.init(mo,error),error);
    for(size_t i=0;i<f.crops.size();i++){
        auto& crop=f.crops[i];sam::Mask predicted;
        require(masker.run(crop.image,predicted,nullptr,1139),masker.lastError());crop.mask=predicted.data;
        require(std::count(crop.mask.begin(),crop.mask.end(),0)>0,"No detected object");
        std::string base=std::string(argv[5])+"/crop"+std::to_string(i);
        require(nn::save_image(crop.image,base+".jpg",90),"Save crop");
        require(nn::save_gray_png(crop.mask.data(),crop.image.width,crop.image.height,base+".png"),"Save mask");
    }
    sam::Mask features;require(masker.runFeatures(f.crops[0].image,features),masker.lastError());
    require(features.data.size()==f.crops[0].mask.size(),"Feature mask dimensions");
    sam::Mask repeated;require(masker.run(f.crops[0].image,repeated,nullptr,1139),masker.lastError());
    size_t different=0,covered=0;
    for(size_t i=0;i<repeated.data.size();i++){different+=repeated.data[i]!=f.crops[0].mask[i];covered+=repeated.data[i]==0;}
    std::cout<<"Repeated mask: "<<different<<" changed pixels / "<<repeated.data.size()<<", removed="<<covered<<std::endl;
    require(different<repeated.data.size()/1000,"Feature pass changed person masking policy");
    for(auto mode:{app::Pano360Mode::Faces,app::Pano360Mode::Corners}){
        app::Pano360Options opt;opt.mode=mode;opt.size=512;int hits=0;
        for(auto v:app::pano360_views(layout,opt)){
            auto rays=app::tracking_target_rays(layout,v,v.width,v.height);
            app::project_tracking_masks(rays,f,mask);
            auto removed=std::count(mask.begin(),mask.end(),0);hits+=removed>0;
            nn::Image canvas;canvas.width=layout.canvasW();canvas.height=layout.canvasH();canvas.data.resize((size_t)canvas.width*canvas.height*3);
            bool have_tracks=true;
            for(int row=0;row<2;row++){
                auto track=nn::load_image(std::string(argv[5])+"/track"+std::to_string(row)+".png");
                if(track.empty()){have_tracks=false;break;}
                for(auto slice:app::eac360_slices(layout))for(int y=0;y<layout.track_h;y++)
                    std::copy_n(track.data.data()+((size_t)y*track.width+slice.src_x)*3,slice.width*3,
                        canvas.data.data()+((size_t)(row*layout.track_h+y)*canvas.width+slice.dst_x)*3);
            }
            if(have_tracks){
                app::Pano360Remap remap;app::pano360_remap(layout,v,remap);
                nn::Image overlay;overlay.width=v.width;overlay.height=v.height;overlay.data.resize((size_t)v.width*v.height*3);
                app::pano360_apply(remap,canvas.data.data(),canvas.width,canvas.height,0,overlay.data.data());
                for(size_t i=0;i<mask.size();i++)if(!mask[i]){overlay.data[i*3]=255;overlay.data[i*3+1]/=2;overlay.data[i*3+2]/=2;}
                nn::save_image(overlay,std::string(argv[5])+"/overlay-"+(mode==app::Pano360Mode::Faces?"face-":"corner-")+v.dir+".jpg",90);
            }
            std::string path=std::string(argv[5])+"/"+(mode==app::Pano360Mode::Faces?"face-":"corner-")+v.dir+".png";
            require(nn::save_gray_png(mask.data(),v.width,v.height,path),"Save projected mask");
            std::cout<<path<<" removed="<<removed<<'\n';
        }
        require(hits>=2,"Expected masks in multiple dataset views");
    }
    std::cout<<"Real-video SAM3 bridge test passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
