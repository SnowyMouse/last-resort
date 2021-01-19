// SPDX-License-Identifier: GPL-3.0-only

#include <invader/command_line_option.hpp>
#include <invader/version.hpp>
#include <invader/printf.hpp>
#include <invader/file/file.hpp>
#include <invader/tag/parser/parser_struct.hpp>
#include <invader/tag/parser/parser.hpp>
#include <invader/tag/hek/header.hpp>
#include <invader/sound/sound_encoder.hpp>
#include <invader/sound/sound_reader.hpp>
#include <invader/bitmap/color_plate_pixel.hpp>
#include <invader/bitmap/bitmap_encode.hpp>
#include <invader/tag/hek/class/bitmap.hpp>
#include <optional>
#include <filesystem>

enum LastResortAction {
    LAST_RESORT_ACTION_HUD_METER_SWAP,
    LAST_RESORT_ACTION_MULTIPURPOSE_GBX_TO_XBOX,
    LAST_RESORT_ACTION_MULTIPURPOSE_XBOX_TO_GBX,
    LAST_RESORT_ACTION_BITMAP_PASSTHROUGH,
    LAST_RESORT_ACTION_SOUND_TO_XBOX_ADPCM
};

using PreferredFormat = std::variant<Invader::HEK::BitmapDataFormat, Invader::HEK::BitmapFormat>;

void iterate_through_bitmap_tag(Invader::Parser::Bitmap *bitmap, const std::optional<PreferredFormat> &force_format, bool dither, void (*modify_pixel)(Invader::ColorPlatePixel &pixel)) {
    if(bitmap == nullptr) {
        eprintf_error("Invalid tag provided for this action");
        throw std::exception();
    }
    
    bool require_lossless_input = bitmap->compressed_color_plate_data.size() > 0;
    
    std::vector<std::byte> new_bitmap_data;
    for(auto &i : bitmap->bitmap_data) {
        if(i.type != Invader::HEK::BitmapDataType::BITMAP_DATA_TYPE_2D_TEXTURE) {
            eprintf_error("Non-2D textures aren't supported yet");
            throw std::exception();
        }
        
        auto size_of_bitmap = Invader::HEK::size_of_bitmap(i.width, i.height, i.depth, i.mipmap_count, i.format, i.type);
        
        if(i.pixel_data_offset >= bitmap->processed_pixel_data.size() || size_of_bitmap > bitmap->processed_pixel_data.size() || i.pixel_data_offset + size_of_bitmap > bitmap->processed_pixel_data.size()) {
            eprintf_error("Bitmap tag invalid - bitmap data out of bounds");
            throw std::exception();
        }
        
        if(require_lossless_input && i.format != Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_X8R8G8B8 && i.format != Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_A8R8G8B8 && i.format != Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_A8 && i.format != Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_Y8 && i.format != Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_A8Y8 && i.format != Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_AY8) {
            eprintf_error("One or more bitmaps is in a lossy format, but there is color plate data!");
            eprintf_error("Converting from a lossy format should NOT be done if there is color plate data.");
            eprintf_error("Use `invader-bitmap -R -F 32-bit` to regenerate this bitmap tag, first.");
            std::exit(EXIT_FAILURE);
        }
        
        auto *data = bitmap->processed_pixel_data.data() + i.pixel_data_offset;
        std::size_t mw = i.width;
        std::size_t mh = i.height;
        std::size_t md = i.depth;
        std::size_t ml = 1;
        
        std::vector<std::byte> new_data;
        
        // Get each mipmap
        std::vector<std::vector<std::byte>> mipmaps;
        for(std::size_t m = 0; m <= i.mipmap_count; m++) {
            mipmaps.emplace_back(Invader::BitmapEncode::encode_bitmap(data, i.format, Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_A8R8G8B8, mw, mh));
            mw = std::max(mw / 2, ml);
            mh = std::max(mh / 2, ml);
            md = std::max(md / 2, ml);
        }
        
        // Reset the variables
        mw = i.width;
        mh = i.height;
        md = i.depth;
        
        if(force_format.has_value()) {
            auto &value = *force_format;
            auto *force_format = std::get_if<Invader::HEK::BitmapDataFormat>(&value);
            if(force_format) {
                i.format = *force_format;
            }
            else {
                auto *force_format_type = std::get_if<Invader::HEK::BitmapFormat>(&value);
                if(force_format_type) {
                    auto &meme = *force_format_type;
                    
                    // Determine alpha depth
                    bool alpha_exists = false;
                    bool alpha_multi_bit = false;
                    
                    bool white = true;
                    bool ay8 = true;
                    
                    for(auto &m : mipmaps) {
                        auto *start = reinterpret_cast<Invader::ColorPlatePixel *>(m.data());
                        auto *end = reinterpret_cast<Invader::ColorPlatePixel *>(m.data() + m.size());
                        
                        for(auto *p = start; p < end; p++) {
                            if(p->alpha != 0xFF) {
                                alpha_exists = true;
                                if(p->alpha != 0) {
                                    alpha_multi_bit = true;
                                    goto spaghetti_exit_mipmaps;
                                }
                            }
                            
                            white = white && p->red == 0xFF && p->green == 0xFF && p->blue == 0xFF;
                            ay8 = ay8 && p->convert_to_a8() == p->convert_to_y8();
                        }
                    }
                    
                    // Check the format
                    spaghetti_exit_mipmaps:
                    switch(meme) {
                        case Invader::HEK::BitmapFormat::BITMAP_FORMAT_MONOCHROME:
                            if(!alpha_exists) {
                                i.format = Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_Y8;
                            }
                            else if(white) {
                                i.format = Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_A8;
                            }
                            else if(ay8) {
                                i.format = Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_AY8;
                            }
                            else {
                                i.format = Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_A8Y8;
                            }
                            break;
                        case Invader::HEK::BitmapFormat::BITMAP_FORMAT_DXT1:
                            i.format = Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT1;
                            break;
                        case Invader::HEK::BitmapFormat::BITMAP_FORMAT_DXT3:
                            i.format = alpha_exists ? Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT3 : Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT1;
                            break;
                        case Invader::HEK::BitmapFormat::BITMAP_FORMAT_DXT5:
                            i.format = alpha_exists ? Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT5 : Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT1;
                            break;
                        case Invader::HEK::BitmapFormat::BITMAP_FORMAT_32_BIT:
                            i.format = alpha_exists ? Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_A8R8G8B8 : Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_X8R8G8B8;
                            break;
                        case Invader::HEK::BitmapFormat::BITMAP_FORMAT_16_BIT:
                            i.format = alpha_exists ? (alpha_multi_bit ? Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_A4R4G4B4 : Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_A1R5G5B5) 
                                                    : Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_R5G6B5;
                            break;
                        case Invader::HEK::BitmapFormat::BITMAP_FORMAT_ENUM_COUNT:
                            std::terminate();
                    }
                }
                else {
                    eprintf_error("what");
                    std::terminate();
                }
            }
            
            // Set palettized flag if needed
            if(i.format == Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_P8_BUMP) {
                i.flags |= Invader::HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_PALETTIZED;
            }
            else {
                i.flags &= ~Invader::HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_PALETTIZED;
            }
            
            // Set compressed flag if needed
            if(i.format == Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT1 || i.format == Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT3 || i.format == Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_DXT5) {
                i.flags |= Invader::HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_COMPRESSED;
            }
            else {
                i.flags &= ~Invader::HEK::BitmapDataFlagsFlag::BITMAP_DATA_FLAGS_FLAG_COMPRESSED;
            }
        }
        
        for(std::size_t m = 0; m <= i.mipmap_count; m++) {
            auto size_of_mipmap = Invader::HEK::size_of_bitmap(mw, mh, md, 0, i.format, i.type);
            auto &converted = mipmaps[m];
            
            auto *pixel_start = reinterpret_cast<Invader::ColorPlatePixel *>(converted.data());
            auto *pixel_end = reinterpret_cast<Invader::ColorPlatePixel *>(converted.data() + converted.size());
            
            for(auto *p = pixel_start; p < pixel_end; p++) {
                modify_pixel(*p);
            }
            
            auto converted_back = Invader::BitmapEncode::encode_bitmap(converted.data(), Invader::HEK::BitmapDataFormat::BITMAP_DATA_FORMAT_A8R8G8B8, i.format, mw, mh, dither, dither, dither, dither);
            new_data.insert(new_data.end(), converted_back.begin(), converted_back.end());
            mw = std::max(mw / 2, ml);
            mh = std::max(mh / 2, ml);
            md = std::max(md / 2, ml);
            data += size_of_mipmap;
        }
        
        i.pixel_data_offset = new_bitmap_data.size();
        new_bitmap_data.insert(new_bitmap_data.end(), new_data.begin(), new_data.end());
    }
    
    bitmap->processed_pixel_data = new_bitmap_data;
    
    bitmap->compressed_color_plate_data.clear(); // clear this in case it isn't -.-
    
    oprintf_success("Modified %zu bitmap%s", bitmap->bitmap_data.size(), bitmap->bitmap_data.size() == 1 ? "" : "s");
}

void hud_meter_swap(Invader::Parser::Bitmap *bitmap, const std::optional<PreferredFormat> &force_format, bool dither) {
    iterate_through_bitmap_tag(bitmap, force_format, dither, [](Invader::ColorPlatePixel &pixel) {
        std::uint8_t mask = pixel.convert_to_y8();
        std::uint8_t meter = pixel.alpha;
        
        pixel.alpha = mask;
        pixel.red = meter;
        pixel.green = meter;
        pixel.blue = meter;
    });
}

void multi_gbx_to_xbox(Invader::Parser::Bitmap *bitmap, const std::optional<PreferredFormat> &force_format, bool dither) {
    iterate_through_bitmap_tag(bitmap, force_format, dither, [](Invader::ColorPlatePixel &pixel) {
        Invader::ColorPlatePixel new_pixel;
        new_pixel.green = pixel.green; // self illumination is passed through
        new_pixel.alpha = 0xFF; // pixel.red; // auxilary is memed to 0xFF because DXT1                                                                                                           
        new_pixel.red = pixel.blue; // detail/specular is memed
        new_pixel.blue = pixel.alpha; // color change is memed 
        pixel = new_pixel;
    });
}

void multi_xbox_to_gbx(Invader::Parser::Bitmap *bitmap, const std::optional<PreferredFormat> &force_format, bool dither) {
    iterate_through_bitmap_tag(bitmap, force_format, dither, [](Invader::ColorPlatePixel &pixel) {
        Invader::ColorPlatePixel new_pixel;
        new_pixel.green = pixel.green; // self illumination is passed through
        new_pixel.red = 0x00; // pixel.alpha; // auxilary is memed to 0x00
        new_pixel.blue = pixel.red; // detail/specular is memed
        new_pixel.alpha = pixel.blue; // color change is memed 
        pixel = new_pixel;
    });
}

bool sound_to_xbox_adpcm(Invader::Parser::Sound *sound) {
    if(sound == nullptr) {
        eprintf_error("Invalid tag provided for this action");
        throw std::exception();
    }
    
    std::size_t converted = 0;
    sound->format = Invader::HEK::SoundFormat::SOUND_FORMAT_XBOX_ADPCM;
    std::size_t channel_count = sound->channel_count == Invader::HEK::SoundChannelCount::SOUND_CHANNEL_COUNT_MONO ? 1 : 2;
    std::size_t sample_rate = sound->sample_rate == Invader::HEK::SoundSampleRate::SOUND_SAMPLE_RATE_22050_HZ ? 22050 : 44100;
    bool split = sound->flags & Invader::HEK::SoundFlagsFlag::SOUND_FLAGS_FLAG_SPLIT_LONG_SOUND_INTO_PERMUTATIONS;
    
    for(auto &i : sound->pitch_ranges) {
        std::vector<Invader::Parser::SoundPermutation> permutations_memes;
        
        auto re_encode_real_permutation = [&converted, &split, &channel_count, &sample_rate, &i, &permutations_memes](std::size_t permutation) {
            auto &base_permutation = i.permutations[permutation];
            auto &new_permutation = permutations_memes.emplace_back(base_permutation);
            auto format = new_permutation.format;
            new_permutation.samples.clear();
            new_permutation.buffer_size = 0;
            new_permutation.format = Invader::HEK::SoundFormat::SOUND_FORMAT_XBOX_ADPCM;
            
            // Append it!
            auto append_permutation = [&channel_count, &sample_rate, &new_permutation, &format](auto &permutation) {
                std::vector<std::byte> samples;
                
                switch(format) {
                    case Invader::HEK::SoundFormat::SOUND_FORMAT_16_BIT_PCM:
                        samples = Invader::SoundEncoder::encode_to_xbox_adpcm(Invader::SoundReader::sound_from_16_bit_pcm_big_endian(permutation.samples.data(), permutation.samples.size(), channel_count, sample_rate).pcm, 16, channel_count);
                        break;
                    case Invader::HEK::SoundFormat::SOUND_FORMAT_OGG_VORBIS: {
                        auto sound = Invader::SoundReader::sound_from_ogg(permutation.samples.data(), permutation.samples.size());
                        samples = Invader::SoundEncoder::encode_to_xbox_adpcm(sound.pcm, sound.bits_per_sample, channel_count);
                        break;
                    }
                    case Invader::HEK::SoundFormat::SOUND_FORMAT_XBOX_ADPCM:
                        samples = permutation.samples;
                        break;
                    default:
                        eprintf_error("Unknown format");
                        throw std::exception();
                }
                
                new_permutation.samples.insert(new_permutation.samples.end(), samples.begin(), samples.end());
            };
            
            // Done
            std::size_t next_permutation = permutation;
            do {
                if(split && next_permutation > i.permutations.size()) {
                    eprintf_error("Next permutation is out of bounds");
                    throw std::exception();
                }
                
                auto &p = i.permutations[next_permutation];
                append_permutation(p);
                next_permutation = p.next_permutation_index;
            } while(split && next_permutation != NULL_INDEX);
            
            if(base_permutation.format != Invader::HEK::SoundFormat::SOUND_FORMAT_XBOX_ADPCM) {
                converted++;
            }
        };
        
        // First pass: go through each real permutation
        if(split) {
            if(i.actual_permutation_count > i.permutations.size()) {
                eprintf_error("Actual permutation count for %s is wrong", i.name.string);
                throw std::exception();
            }
            
            for(std::size_t j = 0; j < i.actual_permutation_count; j++) {
                re_encode_real_permutation(j);
            }
        }
        
        // If we don't have them split into permutations, just go throguh all of them
        else {
            for(std::size_t j = 0; j < i.permutations.size(); j++) {
                re_encode_real_permutation(j);
            }
        }
        
        i.permutations.clear();
        
        // Copy in new permutations
        i.permutations = std::move(permutations_memes);
        
        // Now split them!
        if(split) {
            for(std::size_t j = 0; j < i.actual_permutation_count; j++) {
                auto samples = std::move(i.permutations[j].samples);
                auto *sample_data = samples.data();
                auto sample_size = samples.size();
                auto template_sound = i.permutations[j];
                
                auto *permutation_to_modify = &i.permutations[j];
                static const constexpr std::size_t max_permutation_bytes = 65520; // precomputed
                
                // Add each one at a time
                for(std::size_t q = 0; q < sample_size; q += max_permutation_bytes) {
                    if(q) {
                        permutation_to_modify->next_permutation_index = i.permutations.size();
                        permutation_to_modify = &i.permutations.emplace_back(template_sound);
                    }
                    permutation_to_modify->samples = std::vector<std::byte>(sample_data + q, sample_data + q + std::min(sample_size - q, max_permutation_bytes));
                    permutation_to_modify->next_permutation_index = NULL_INDEX;
                }
            }
        }
    }
    
    if(converted > 0) {
        oprintf_success("Converted %zu permutation%s into Xbox ADPCM", converted, converted == 1 ? "" : "s");
        return true;
    }
    else {
        return false;
    }
}

int main(int argc, const char **argv) {
    using namespace Invader;
    
    struct LastResortOptions {
        std::optional<LastResortAction> action;
        bool use_filesystem_path = false;
        bool dither = false;
        std::filesystem::path tags = "tags";
        std::optional<std::filesystem::path> output_tags;
        std::optional<PreferredFormat> force_format;
    } last_resort_options;
    
    std::vector<CommandLineOption> options;
    options.emplace_back("type", 'T', 1, "Set the type of action to take. Can be: hud-meter-swap, multi-gbx-to-xbox, multi-xbox-to-gbx, sound-to-xbox-adpcm, bitmap-passthrough", "<action>");
    options.emplace_back("bitmap-format", 'F', 1, "Force the bitmap format to be something else (can be dxt1, dxt3, dxt5, monochrome, 32-bit, 16-bit, a8r8g8b8, x8r8g8b8, r5g6b5, a1r5g5b5, a4r4g4b4, a8, y8, ay8, a8y8, p8)", "<format>");
    options.emplace_back("fs-path", 'P', 0, "Use a filesystem path for the tag.");
    options.emplace_back("dither", 'd', 0, "Use dithering when possible.");
    options.emplace_back("tags", 't', 1, "Set the tags directory.", "<dir>");
    options.emplace_back("output-tags", 'o', 1, "Set the output tags directory. By default, the input tags directory is used.", "<dir>");

    static constexpr char DESCRIPTION[] = "Convince a tag to work with the Xbox version of Halo when nothing else works.";
    static constexpr char USAGE[] = "[options] -T <action> -o <dir> <tag.class>";
    
    auto remaining_arguments = CommandLineOption::parse_arguments<LastResortOptions &>(argc, argv, options, USAGE, DESCRIPTION, 1, 1, last_resort_options, [](char opt, const auto &arguments, auto &last_resort_options) {
        switch(opt) {
            case 'T':
                if(std::strcmp(arguments[0], "hud-meter-swap") == 0) {
                    last_resort_options.action = LastResortAction::LAST_RESORT_ACTION_HUD_METER_SWAP;
                }
                else if(std::strcmp(arguments[0], "multi-gbx-to-xbox") == 0) {
                    last_resort_options.action = LastResortAction::LAST_RESORT_ACTION_MULTIPURPOSE_GBX_TO_XBOX;
                }
                else if(std::strcmp(arguments[0], "multi-xbox-to-gbx") == 0) {
                    last_resort_options.action = LastResortAction::LAST_RESORT_ACTION_MULTIPURPOSE_XBOX_TO_GBX;
                }
                else if(std::strcmp(arguments[0], "sound-to-xbox-adpcm") == 0) {
                    last_resort_options.action = LastResortAction::LAST_RESORT_ACTION_SOUND_TO_XBOX_ADPCM;
                }
                else if(std::strcmp(arguments[0], "bitmap-passthrough") == 0) {
                    last_resort_options.action = LastResortAction::LAST_RESORT_ACTION_BITMAP_PASSTHROUGH;
                }
                else {
                    eprintf_error("Unknown option: %s", arguments[0]);
                    std::exit(EXIT_FAILURE);
                }
                break;
            case 'P':
                last_resort_options.use_filesystem_path = true;
                break;
            case 'd':
                last_resort_options.dither = true;
                break;
            case 'F':
                try {
                    last_resort_options.force_format = Invader::HEK::BitmapDataFormat_from_string(arguments[0]);
                }
                catch(std::exception &) {
                    try {
                        last_resort_options.force_format = Invader::HEK::BitmapFormat_from_string(arguments[0]);
                    }
                    catch(std::exception &) {
                        eprintf_error("Unknown format: %s", arguments[0]);
                        std::exit(EXIT_FAILURE);
                    }
                }
                break;
            case 't':
                last_resort_options.tags = arguments[0];
                break;
            case 'o':
                last_resort_options.output_tags = arguments[0];
                break;
            default:
                break;
        }
    });
    
    if(!last_resort_options.action.has_value()) {
        eprintf_error("No action was specified. Use -h for more information.");
        return EXIT_FAILURE;
    }
    
    if(!last_resort_options.output_tags.has_value()) {
        eprintf_error("No output tags directory was specified. Use -h for more information.");
        return EXIT_FAILURE;
    }
    
    std::string path;
    
    if(last_resort_options.use_filesystem_path) {
        auto path_maybe = Invader::File::file_path_to_tag_path(remaining_arguments[0], std::vector<std::filesystem::path>(&last_resort_options.tags, &last_resort_options.tags + 1), true);
        if(path_maybe.has_value()) {
            path = path_maybe.value();
        }
        else {
            eprintf_error("Failed to find a valid tag %s in the tags directory", remaining_arguments[0]);
            return EXIT_FAILURE;
        }
    }
    else {
        path = File::halo_path_to_preferred_path(remaining_arguments[0]);
    }
    
    // Open that
    std::filesystem::path file_path = last_resort_options.tags / Invader::File::halo_path_to_preferred_path(path);
    auto file_data = Invader::File::open_file(file_path);
    if(!file_data.has_value()) {
        eprintf_error("Failed to open %s", file_path.string().c_str());
        return EXIT_FAILURE;
    }
    
    try {
        auto tag_file = Invader::Parser::ParserStruct::parse_hek_tag_file(file_data->data(), file_data->size());
        switch(*last_resort_options.action) {
            case LastResortAction::LAST_RESORT_ACTION_HUD_METER_SWAP:
                hud_meter_swap(dynamic_cast<Invader::Parser::Bitmap *>(tag_file.get()), last_resort_options.force_format, last_resort_options.dither);
                break;
            case LastResortAction::LAST_RESORT_ACTION_MULTIPURPOSE_GBX_TO_XBOX:
                multi_gbx_to_xbox(dynamic_cast<Invader::Parser::Bitmap *>(tag_file.get()), last_resort_options.force_format, last_resort_options.dither);
                break;
            case LastResortAction::LAST_RESORT_ACTION_MULTIPURPOSE_XBOX_TO_GBX:
                multi_xbox_to_gbx(dynamic_cast<Invader::Parser::Bitmap *>(tag_file.get()), last_resort_options.force_format, last_resort_options.dither);
                break;
            case LastResortAction::LAST_RESORT_ACTION_BITMAP_PASSTHROUGH:
                iterate_through_bitmap_tag(dynamic_cast<Invader::Parser::Bitmap *>(tag_file.get()), last_resort_options.force_format, last_resort_options.dither, [](auto &) {});
                break;
            case LastResortAction::LAST_RESORT_ACTION_SOUND_TO_XBOX_ADPCM:
                if(!sound_to_xbox_adpcm(dynamic_cast<Invader::Parser::Sound *>(tag_file.get()))) {
                    oprintf("No conversion necessary; sound tag already Xbox ADPCM\n");
                    std::exit(EXIT_SUCCESS);
                }
                break;
        }
        
        auto tag_file_saved = tag_file->generate_hek_tag_data(reinterpret_cast<const Invader::HEK::TagFileHeader *>(file_data->data())->tag_class_int);
        
        auto output_file_path = last_resort_options.output_tags.value() / Invader::File::halo_path_to_preferred_path(path);
        
        std::error_code ec;
        std::filesystem::create_directories(output_file_path.parent_path(), ec); // make dirs
        
        if(!Invader::File::save_file(output_file_path, tag_file_saved)) {
            eprintf_error("Failed to write to %s", output_file_path.string().c_str());
            return EXIT_FAILURE;
        }
    }
    catch(std::exception &e) {
        eprintf_error("Failed to parse %s", file_path.string().c_str());
        return EXIT_FAILURE;
    }
}
