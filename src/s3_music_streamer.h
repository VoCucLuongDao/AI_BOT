#pragma once

#include <string>
#include <vector>

class S3MusicStreamer {
public:
    static bool ListTracks(std::vector<std::string>* tracks);
    static bool Start(const std::string& track_name);
    static void Stop();
    static void SetPaused(bool paused);
    static bool IsActive();
};
