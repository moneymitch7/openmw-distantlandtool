// OpenMGE XE openmw-distantlandtool: makes simpler distant versions of the models placed outside, for object paging's
// distant chunks (Misc::ResourceHelpers::getLODMeshName finds them as <model>_dist_0.osgb). Run it again after
// changing mods; without it, or for a model it left alone, the chunks use the full model as before.

#include "simplify.hpp"

#include <components/debug/debugging.hpp>
#include <components/debug/debuglog.hpp>
#include <components/esm3/cellref.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/esmloader/esmdata.hpp>
#include <components/esmloader/lessbyid.hpp>
#include <components/esmloader/load.hpp>
#include <components/esmloader/record.hpp>
#include <components/fallback/fallback.hpp>
#include <components/fallback/validate.hpp>
#include <components/files/collections.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/files/conversion.hpp>
#include <components/files/multidircollection.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/platform/platform.hpp>
#include <components/resource/bgsmfilemanager.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/settings/values.hpp>
#include <components/toutf8/toutf8.hpp>
#include <components/version/version.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/registerarchives.hpp>

#include <osgDB/Registry>

#include <boost/program_options.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace DistantLandTool
{
    namespace
    {
        namespace bpo = boost::program_options;

        using StringsVector = std::vector<std::string>;

        constexpr std::string_view applicationName = "DistantLandTool";

        // the list of what the last run wrote, in the output folder: removed before writing anew
        constexpr std::string_view manifestName = "distantland-files.txt";

        bpo::options_description makeOptionsDescription()
        {
            bpo::options_description result;
            auto addOption = result.add_options();
            addOption("help", "print help message");

            addOption("version", "print version information and quit");

            addOption("data",
                bpo::value<Files::MaybeQuotedPathContainer>()
                    ->default_value(Files::MaybeQuotedPathContainer(), "data")
                    ->multitoken()
                    ->composing(),
                "set data directories (later directories have higher priority)");

            addOption("data-local",
                bpo::value<Files::MaybeQuotedPathContainer::value_type>()->default_value(
                    Files::MaybeQuotedPathContainer::value_type(), ""),
                "set local data directory (highest priority)");

            addOption("fallback-archive",
                bpo::value<StringsVector>()
                    ->default_value(StringsVector(), "fallback-archive")
                    ->multitoken()
                    ->composing(),
                "set fallback BSA archives (later archives have higher priority)");

            addOption("content",
                bpo::value<StringsVector>()->default_value(StringsVector(), "")->multitoken()->composing(),
                "content file(s): esm/esp, or omwgame/omwaddon/omwscripts");

            addOption("encoding", bpo::value<std::string>()->default_value("win1252"),
                "Character encoding used in OpenMW game messages");

            addOption("fallback",
                bpo::value<Fallback::FallbackMap>()
                    ->default_value(Fallback::FallbackMap(), "")
                    ->multitoken()
                    ->composing(),
                "fallback values");

            addOption("threads",
                bpo::value<std::size_t>()->default_value(
                    std::max<std::size_t>(std::thread::hardware_concurrency() - 1, 1)),
                "number of threads for parallel processing");

            addOption("output", bpo::value<Files::MaybeQuotedPath>()->default_value(Files::MaybeQuotedPath(), ""),
                "folder to write the distant models to, a data folder of the game (default: the local data folder, "
                "data-local)");

            addOption("max-error", bpo::value<float>()->default_value(4.f),
                "how far a simplified shape may stray from the model, in game units");

            addOption("min-part-size", bpo::value<float>()->default_value(8.f),
                "parts of a model smaller than this radius (game units, at the largest scale it is placed with) are "
                "left out");

            addOption("min-object-size", bpo::value<float>()->default_value(100.f),
                "models smaller than this radius (game units, at the largest scale) are skipped: distant chunks "
                "leave them out anyway");

            addOption("model",
                bpo::value<StringsVector>()->default_value(StringsVector(), "")->multitoken()->composing(),
                "only these models (paths in the data folders, as meshes/x/y.nif), at scale 1, each read back after "
                "it is written; for testing");

            addOption("dry-run", bpo::value<bool>()->implicit_value(true)->default_value(false),
                "simplify and report, but write nothing");

            Files::ConfigurationManager::addCommonOptions(result);

            return result;
        }

        struct ModelUse
        {
            float mMaxScale = 0.f;
            std::size_t mCount = 0;
        };

        struct CellRef
        {
            ESM::RecNameInts mType;
            ESM::RefNum mRefNum;
            ESM::RefId mRefId;
            float mScale;
        };

        ESM::RecNameInts getType(const EsmLoader::EsmData& esmData, const ESM::RefId& refId)
        {
            const auto it = std::lower_bound(
                esmData.mRefIdTypes.begin(), esmData.mRefIdTypes.end(), refId, EsmLoader::LessById{});
            if (it == esmData.mRefIdTypes.end() || it->mId != refId)
                return {};
            return it->mType;
        }

        std::vector<CellRef> loadCellRefs(
            const ESM::Cell& cell, const EsmLoader::EsmData& esmData, ESM::ReadersCache& readers)
        {
            std::vector<EsmLoader::Record<CellRef>> cellRefs;
            for (std::size_t i = 0; i < cell.mContextList.size(); i++)
            {
                ESM::ReadersCache::BusyItem reader = readers.get(static_cast<std::size_t>(cell.mContextList[i].index));
                cell.restore(*reader, static_cast<int>(i));
                ESM::CellRef cellRef;
                bool deleted = false;
                while (ESM::Cell::getNextRef(*reader, cellRef, deleted))
                {
                    const ESM::RecNameInts type = getType(esmData, cellRef.mRefID);
                    if (type == ESM::RecNameInts{})
                        continue;
                    cellRefs.push_back(EsmLoader::Record<CellRef>{
                        deleted, CellRef{ type, cellRef.mRefNum, std::move(cellRef.mRefID), cellRef.mScale } });
                }
            }
            const auto getKey = [](const EsmLoader::Record<CellRef>& v) -> ESM::RefNum { return v.mValue.mRefNum; };
            return EsmLoader::prepareRecords(cellRefs, getKey);
        }

        // The models placed outside that object paging draws in distant chunks (statics, activators, doors), with
        // the largest scale each is placed with.
        std::map<VFS::Path::Normalized, ModelUse> collectModels(
            const EsmLoader::EsmData& esmData, ESM::ReadersCache& readers)
        {
            std::map<VFS::Path::Normalized, ModelUse> result;
            std::size_t cells = 0;
            for (const ESM::Cell& cell : esmData.mCells)
            {
                if (!cell.isExterior())
                    continue;
                ++cells;
                for (const CellRef& ref : loadCellRefs(cell, esmData, readers))
                {
                    if (ref.mType != ESM::REC_STAT && ref.mType != ESM::REC_ACTI && ref.mType != ESM::REC_DOOR)
                        continue;
                    const VFS::Path::NormalizedView model = EsmLoader::getModel(esmData, ref.mRefId, ref.mType);
                    if (model.empty())
                        continue;
                    ModelUse& use = result[Misc::ResourceHelpers::correctMeshPath(model)];
                    use.mMaxScale = std::max(use.mMaxScale, ref.mScale > 0.f ? ref.mScale : 1.f);
                    ++use.mCount;
                }
            }
            Log(Debug::Info) << "Found " << result.size() << " models placed in " << cells << " cells outside";
            return result;
        }

        // meshes/x/y.nif -> meshes/x/y_dist_0.osgb
        std::string getDistantName(const VFS::Path::Normalized& model)
        {
            std::string name(model.value());
            const std::size_t dot = name.find_last_of('.');
            if (dot != std::string::npos && name.find('/', dot) == std::string::npos)
                name.erase(dot);
            return name + "_dist_0.osgb";
        }

        void removePreviousFiles(const std::filesystem::path& output)
        {
            const std::filesystem::path manifest = output / manifestName;
            std::ifstream stream(manifest);
            if (!stream)
                return;
            std::size_t removed = 0;
            std::string line;
            while (std::getline(stream, line))
            {
                // only what this tool writes
                if (line.empty() || !line.ends_with("_dist_0.osgb") || line.find("..") != std::string::npos)
                    continue;
                std::error_code ec;
                if (std::filesystem::remove(output / Files::pathFromUnicodeString(line), ec))
                    ++removed;
            }
            stream.close();
            std::filesystem::remove(manifest);
            Log(Debug::Info) << "Removed " << removed << " distant models of the last run";
        }

        int runDistantLandTool(int argc, char* argv[])
        {
            Platform::init();

            bpo::options_description desc = makeOptionsDescription();

            bpo::parsed_options options = bpo::command_line_parser(argc, argv).options(desc).allow_unregistered().run();
            bpo::variables_map variables;

            bpo::store(options, variables);
            bpo::notify(variables);

            if (variables.find("help") != variables.end())
            {
                Debug::getRawStdout() << desc << std::endl;
                return 0;
            }

            Files::ConfigurationManager config;
            config.processPaths(variables, std::filesystem::current_path());
            config.readConfiguration(variables, desc);

            Debug::setupLogging(config.getLogPath(), applicationName);

            const std::string encoding(variables["encoding"].as<std::string>());
            Log(Debug::Info) << ToUTF8::encodingUsingMessage(encoding);
            ToUTF8::Utf8Encoder encoder(ToUTF8::calculateEncoding(encoding));

            Files::PathContainer dataDirs(asPathContainer(variables["data"].as<Files::MaybeQuotedPathContainer>()));

            const auto local = variables["data-local"].as<Files::MaybeQuotedPathContainer::value_type>();
            if (!local.empty())
                dataDirs.push_back(local);

            config.filterOutNonExistingPaths(dataDirs);

            std::filesystem::path output = variables["output"].as<Files::MaybeQuotedPath>();
            if (output.empty())
                output = local.empty() ? config.getUserDataPath() / "data" : std::filesystem::path(local);

            const auto& resDir = variables["resources"].as<Files::MaybeQuotedPath>();
            Log(Debug::Info) << Version::getOpenmwVersionDescription();
            dataDirs.insert(dataDirs.begin(), resDir / "vfs");
            const Files::Collections fileCollections(dataDirs);
            const auto& archives = variables["fallback-archive"].as<StringsVector>();
            StringsVector contentFiles{ "builtin.omwscripts" };
            const auto& configContentFiles = variables["content"].as<StringsVector>();
            contentFiles.insert(contentFiles.end(), configContentFiles.begin(), configContentFiles.end());
            const std::size_t threadsNumber = std::max<std::size_t>(variables["threads"].as<std::size_t>(), 1);
            const bool dryRun = variables["dry-run"].as<bool>();

            SimplifyOptions simplifyOptions;
            simplifyOptions.mMaxError = std::max(variables["max-error"].as<float>(), 0.f);
            simplifyOptions.mMinPartRadius = std::max(variables["min-part-size"].as<float>(), 0.f);
            const float minObjectRadius = std::max(variables["min-object-size"].as<float>(), 0.f);

            Fallback::Map::init(variables["fallback"].as<Fallback::FallbackMap>().mMap);

            VFS::Manager vfs;
            VFS::registerArchives(&vfs, fileCollections, archives, true, &encoder.getStatelessEncoder());

            Settings::Manager::load(config);

            const StringsVector& onlyModels = variables["model"].as<StringsVector>();
            std::map<VFS::Path::Normalized, ModelUse> models;
            if (onlyModels.empty())
            {
                ESM::ReadersCache readers;
                EsmLoader::Query query;
                query.mLoadActivators = true;
                query.mLoadCells = true;
                query.mLoadDoors = true;
                query.mLoadStatics = true;
                const EsmLoader::EsmData esmData
                    = EsmLoader::loadEsmData(query, contentFiles, fileCollections, readers, &encoder);
                models = collectModels(esmData, readers);
            }
            else
                for (const std::string& model : onlyModels)
                    models[VFS::Path::Normalized(model)] = ModelUse{ 1.f, 1 };

            constexpr double expiryDelay = 0;
            Resource::ImageManager imageManager(&vfs, expiryDelay);
            // only the names of the textures are kept; the game reads them itself
            imageManager.setNamesOnly(true);
            Resource::NifFileManager nifFileManager(&vfs, &encoder.getStatelessEncoder());
            Resource::BgsmFileManager bgsmFileManager(&vfs, expiryDelay);

            Log(Debug::Info) << "Writing distant models to " << output << (dryRun ? " (dry run: nothing written)" : "");
            if (!dryRun)
            {
                std::filesystem::create_directories(output);
                removePreviousFiles(output);
            }

            osgDB::ReaderWriter* writer = osgDB::Registry::instance()->getReaderWriterForExtension("osgb");
            if (writer == nullptr && !dryRun)
            {
                Log(Debug::Error) << "No OSG writer for .osgb files (the osgdb_osg plugin): nothing can be written";
                return 1;
            }
            osg::ref_ptr<osgDB::Options> writeOptions = new osgDB::Options("WriteImageHint=UseExternal");

            std::vector<std::pair<VFS::Path::Normalized, ModelUse>> work(models.begin(), models.end());
            std::atomic<std::size_t> next = 0;
            std::mutex mutex; // the log of skipped models, the manifest, the totals
            std::mutex writeMutex;
            std::vector<std::string> written;
            std::map<std::string, std::size_t> skipped;
            std::size_t trianglesIn = 0;
            std::size_t trianglesOut = 0;
            std::size_t drawablesIn = 0;
            std::size_t drawablesOut = 0;
            std::size_t small = 0;
            std::size_t failed = 0;
            const auto start = std::chrono::steady_clock::now();

            // A batch at a time, the caches emptied between batches (a load order's models don't fit in memory).
            constexpr std::size_t batchSize = 512;
            for (std::size_t batchStart = 0; batchStart < work.size(); batchStart += batchSize)
            {
                const std::size_t batchEnd = std::min(work.size(), batchStart + batchSize);
                next = batchStart;
                const auto worker = [&] {
                    while (true)
                    {
                        const std::size_t i = next++;
                        if (i >= batchEnd)
                            return;
                        const auto& [model, use] = work[i];
                        try
                        {
                            // the model as the game builds it from the NIF (without the shaders and the optimizer,
                            // which only matter for drawing it); other formats are left as they are
                            if (model.extension() != VFS::Path::ExtensionView("nif"))
                            {
                                const std::lock_guard lock(mutex);
                                ++skipped["not a NIF"];
                                continue;
                            }
                            const osg::ref_ptr<const osg::Node> node
                                = NifOsg::Loader::load(*nifFileManager.get(model), &imageManager, &bgsmFileManager);
                            if (node->getBound().radius() * use.mMaxScale < minObjectRadius)
                            {
                                const std::lock_guard lock(mutex);
                                ++small;
                                continue;
                            }
                            SimplifyResult result = simplifyModel(*node, use.mMaxScale, simplifyOptions);
                            const std::string distantName = getDistantName(model);
                            if (result.mNode != nullptr && !dryRun)
                            {
                                const std::filesystem::path path = output / Files::pathFromUnicodeString(distantName);
                                std::filesystem::create_directories(path.parent_path());
                                std::ofstream stream(path, std::ios::binary);
                                const std::lock_guard writeLock(writeMutex);
                                const osgDB::ReaderWriter::WriteResult writeResult
                                    = writer->writeNode(*result.mNode, stream, writeOptions);
                                if (!writeResult.success())
                                {
                                    Log(Debug::Warning) << "Failed to write " << path << ": " << writeResult.message();
                                    result.mNode = nullptr;
                                    result.mSkipReason = "not written";
                                }
                            }
                            const std::lock_guard lock(mutex);
                            if (result.mNode != nullptr)
                            {
                                written.push_back(distantName);
                                trianglesIn += result.mTrianglesIn;
                                trianglesOut += result.mTrianglesOut;
                                drawablesIn += result.mDrawablesIn;
                                drawablesOut += result.mDrawablesOut;
                                Log(Debug::Verbose)
                                    << model << ": " << result.mTrianglesIn << " -> " << result.mTrianglesOut
                                    << " triangles, " << result.mDrawablesIn << " -> " << result.mDrawablesOut
                                    << " shapes (placed " << use.mCount << " times)";
                            }
                            else
                            {
                                ++skipped[result.mSkipReason];
                                Log(Debug::Verbose) << model << ": left as it is (" << result.mSkipReason << ")";
                            }
                        }
                        catch (const std::exception& e)
                        {
                            const std::lock_guard lock(mutex);
                            ++failed;
                            Log(Debug::Warning) << "Failed to process " << model << ": " << e.what();
                        }
                    }
                };
                std::vector<std::thread> threads;
                for (std::size_t t = 0; t < threadsNumber; ++t)
                    threads.emplace_back(worker);
                for (std::thread& thread : threads)
                    thread.join();
                nifFileManager.clearCache();
                imageManager.clearCache();
                Log(Debug::Info) << "Processed " << batchEnd << " of " << work.size() << " models";
            }

            // testing: the written models read back as the game reads them
            if (!onlyModels.empty() && !dryRun)
            {
                osg::ref_ptr<osgDB::Options> readOptions = new osgDB::Options;
                for (const std::string& name : written)
                {
                    std::ifstream stream(output / Files::pathFromUnicodeString(name), std::ios::binary);
                    osgDB::ReaderWriter::ReadResult read = writer->readNode(stream, readOptions);
                    if (!read.success() || read.getNode() == nullptr)
                    {
                        Log(Debug::Error) << "Failed to read back " << name << ": " << read.message();
                        continue;
                    }
                    const osg::Node* node = read.getNode();
                    Log(Debug::Info) << "Read back " << name << ": radius " << node->getBound().radius() << ", "
                                     << (node->asGroup() != nullptr ? node->asGroup()->getNumChildren() : 0)
                                     << " shapes";
                }
            }

            if (!dryRun)
            {
                std::sort(written.begin(), written.end());
                std::ofstream manifest(output / manifestName);
                for (const std::string& name : written)
                    manifest << name << '\n';
            }

            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            Log(Debug::Info) << "Made " << written.size() << " distant models in " << seconds << " s: " << trianglesIn
                             << " -> " << trianglesOut << " triangles, " << drawablesIn << " -> " << drawablesOut
                             << " shapes";
            Log(Debug::Info) << small << " models too small to be drawn in distant chunks, " << failed
                             << " failed to load";
            for (const auto& [reason, count] : skipped)
                Log(Debug::Info) << count << " left as they are: " << reason;
            Log(Debug::Info) << "Done";
            return 0;
        }
    }
}

#ifdef ANDROID
extern "C" int SDL_main(int argc, char* argv[])
#else
int main(int argc, char* argv[])
#endif
{
    return Debug::wrapApplication(DistantLandTool::runDistantLandTool, argc, argv, DistantLandTool::applicationName);
}
