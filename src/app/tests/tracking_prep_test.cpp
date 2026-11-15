#include "app/gui/DatasetPrep.h"
#include "app/gui/ReconStamp.h"
#include <filesystem>
#include <iostream>
#include <atomic>
#include <cstdlib>
int main(int argc,char** argv){
 if(argc<7||argc>9)return 2;
 const bool bin_mode=std::string(argv[1]).empty();
 gui::PrepJob job;job.workspace=argv[6];job.gopro_renderer=argv[1];job.gopro_cache_folder=argv[2];job.gopro_tracking=true;
 job.mask_enable=true;job.mask_prompt="person; human shadow";
 if(!bin_mode)job.mask_model_path=argv[4];
 // pano plan: "corners" (the 4-per-fisheye pinhole view set), "faces" or
 // "fisheye"; the other argv leaves stay in place.
 const std::string plan=argc>7?std::string(argv[7]):"corners";
 job.pano.mode=plan=="faces"?app::Pano360Mode::Faces:
               plan=="fisheye"?app::Pano360Mode::Fisheye:app::Pano360Mode::Corners;
 job.pano.size=0;
 // The side the frames were extracted at, when the caller names one.
 if(argc>8)job.pano.size=std::atoi(argv[8]);
 gui::PrepInput input;input.path=argv[3];input.is_video=true;input.video_tracks=2;
 app::pano360_detect(2,4096,1344,{},input.pano360);job.inputs.push_back(input);
 int views=0;
 if(argc>5&&*argv[5]){
  for(auto view:app::pano360_views(input.pano360,job.pano)){
   auto folder=std::filesystem::path(job.workspace)/"images"/view.dir;std::filesystem::create_directories(folder);
   std::filesystem::copy_file(std::filesystem::path(argv[5])/("overlay-face-"+view.dir+".jpg"),folder/"01139.jpg",std::filesystem::copy_options::overwrite_existing);
   ++views;
  }
 }
 gui::write_recon_stamp(job.workspace,gui::frames_stamp(job),gui::kFramesStampFile);
 std::atomic<bool> cancel{false};gui::RunProgress progress;gui::DatasetPrep prep(&progress,gui::RunFilms{},cancel);gui::PrepResult result;std::string error;
 bool ok=prep.run(job,result,error);
 for(auto line:progress.drain())std::cout<<line.text<<'\n';
 if(!ok){std::cerr<<error<<'\n';return 1;}
 int masks=0;
 std::error_code ec;
 for(auto it=std::filesystem::recursive_directory_iterator(std::filesystem::path(job.workspace)/"masks",ec);
     it!=std::filesystem::recursive_directory_iterator();++it)
  if(!it->is_directory(ec))++masks;
 if(!views)for(auto e=std::filesystem::recursive_directory_iterator(std::filesystem::path(job.workspace)/"images",ec);e!=std::filesystem::recursive_directory_iterator();++e)
  if(e->is_directory(ec))++views;
 if(masks<views){std::cerr<<(bin_mode?"bin":"renderer")<<" masks "<<masks<<" < planted views "<<views<<"\n";return 1;}
 std::cout<<"DatasetPrep "<<(bin_mode?"bin":"bridge")<<" passed: "<<result.n_images<<" images, "<<masks<<" masks\n";
 return 0;
}
