#include <fluxcrawl/checkpoint.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv) try {
    if(argc!=2) { return 2; }
    std::size_t rejected=0;
    for(const auto& entry:std::filesystem::directory_iterator(argv[1])) {
        if(entry.path().extension()!=".txt") {continue;}
        std::ifstream file{entry.path()}; const std::string text{std::istreambuf_iterator<char>{file},{}};
        bool failed=false;
        try { const auto saved=fluxcrawl::CheckpointManager::deserialize(text); (void)fluxcrawl::CheckpointManager::serialize(saved); }
        catch(const std::exception&) {failed=true;}
        if(failed==(entry.path().stem()=="valid")) {throw std::runtime_error("checkpoint corpus expectation: "+entry.path().filename().string());}
        rejected+=failed;
    }
    if(rejected<7) {throw std::runtime_error("corruption corpus incomplete");}
    std::cout<<rejected<<" corrupt checkpoints rejected before restore\n";
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
