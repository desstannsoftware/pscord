// Host CLI: extract a debug/fake FIH PKG to a folder (same engine as the console app).
#include "pkg/extract.hpp"
#include "pkg/launch_fix.hpp"
#include "pkg/publish.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr,
                     "usage: %s <pkg> <outdir> [--launch-fix <libSceAmpr.sprx>] [--publish <homebrew_root>]\n",
                     argv[0]);
        return 2;
    }
    const char *pkg = argv[1];
    const char *outdir = argv[2];
    const char *homebrew = nullptr;
    const char *ampr_sprx = nullptr;
    for (int i = 3; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--publish") == 0 && i + 1 < argc)
            homebrew = argv[++i];
        else if (std::strcmp(argv[i], "--launch-fix") == 0 && i + 1 < argc)
            ampr_sprx = argv[++i];
    }

    pkg::ExtractOptions opts;
    auto result = pkg::extract_package(
        pkg, outdir, opts,
        [](const pkg::ExtractProgress &p) {
            std::fprintf(stderr, "\r[%s] %3.0f%% %s          ", p.stage.c_str(), p.fraction * 100.0,
                         p.current_file.c_str());
            std::fflush(stderr);
        },
        {},
        [](const std::string &m) { std::fprintf(stderr, "\n%s\n", m.c_str()); });

    std::fprintf(stderr, "\n");
    if (!result.ok)
    {
        std::fprintf(stderr, "extract failed: %s\n", result.error.c_str());
        return 1;
    }
    std::printf("ok files=%d out=%s\n", result.extracted_file_count, result.output_directory.c_str());
    std::printf("title_id=%s\n", result.fih.title_id.c_str());

    if (ampr_sprx)
    {
        std::string fix_error;
        if (!pkg::apply_launch_fix(outdir, &fix_error, ampr_sprx))
        {
            std::fprintf(stderr, "launch fix failed: %s\n", fix_error.c_str());
            return 1;
        }
        std::printf("launch fix applied\n");
    }

    if (homebrew)
    {
        pkg::PublishOptions popts;
        popts.homebrew_root = homebrew;
        auto pub = pkg::publish_folder(outdir, result.fih.title_id, popts);
        if (!pub.ok)
        {
            std::fprintf(stderr, "publish failed: %s\n", pub.error.c_str());
            return 1;
        }
        std::printf("published %s\n", pub.destination.c_str());
    }
    return 0;
}
