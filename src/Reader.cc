#include "DRS4lib/DataFormat.h"
#include "DRS4lib/Reader.h"

using namespace drs4;

void Reader::addModule(const std::string& filename, size_t module_id, const ModuleCalibrations& calibrations) {
  files_readers_.insert(std::make_pair(module_id, ModuleFileReader(filename, calibrations)));
}

bool Reader::next(GlobalEvent& event) {
  event.clear();
  ssize_t other_event_number = -999;
  for (auto& [module_id, file_reader] : files_readers_) {
    Event module_event;
    if (!file_reader.next(module_event))
      return false;
    if (other_event_number < 0)
      other_event_number = module_event.header().eventNumber();
    else if (module_event.header().eventNumber() != other_event_number)
      throw std::runtime_error("Lost synchronisation between the module readers. Event number from other readers: " +
                               std::to_string(module_event.header().eventNumber()) +
                               " != " + std::to_string(other_event_number) + ".");
    event.addModuleEvent(module_id, module_event);
  }
  return true;
}

Reader::ModuleFileReader::ModuleFileReader(const std::string& filename, const ModuleCalibrations& calibrations)
    : calibrations_(calibrations) {
  //**********************************************************
  // Check if has valid input files, otherwise exit with error
  //**********************************************************
  if (std::ifstream ifile(filename); !ifile)
    throw std::runtime_error("!USAGE! Input file '" + filename + "' does not exist. Please enter valid file name");
  file_.reset(std::fopen(filename.data(), "r"));
}

bool Reader::ModuleFileReader::next(Event& event) {
  // check for end of files
  if (feof(file_.get()))
    return false;
  //if (iEvent % 1000 == 0)
  //  std::cout << "Event " << iEvent << "\n";

  std::array<uint32_t, 4> event_header_words;
  for (size_t i = 0; i < event_header_words.size(); ++i)
    const auto dummy = fread(&event_header_words.at(i), sizeof(uint32_t), 1, file_.get());
  const EventHeader event_header(event_header_words);
  event = Event(event_header);

  //************************************
  // Loop over channel groups
  //************************************
  for (size_t group = 0; group < event.header().groupMask().size(); ++group) {
    if (!event.header().groupMask().at(group))
      continue;
    // Read group header
    uint32_t header_payload;
    const auto dummy = fread(&header_payload, sizeof(uint32_t), 1, file_.get());
    auto& group_info = event.addGroup(ChannelGroup(header_payload));
    // uint16_t tcn = (event_header >> 20) & 0xfff; // trigger counter bin
    const auto tcn = group_info.triggerCounter();

    // Check if all channels were active (if 8 channels active return 3072)
    const auto nsample = group_info.numSamples();
    // std::cout << " Group =  " << group << "   samples = " << nsample << std::endl;

    // Define time coordinate
    std::vector<float> group_times{0.};
    for (size_t i = 1; i < 1024; i++)
      group_times.emplace_back(calibrations_.groupCalibrations(group).timeCalibrations().at((i - 1 + tcn) % 1024) *
                                   tscale_.at(group_info.frequency()) +
                               group_times.at(i - 1));
    group_info.setTimes(group_times);

    //************************************
    // Read sample info for group
    //************************************

    std::array<uint32_t, 3> packed_sample_frame;
    std::vector<std::vector<uint16_t> > channel_samples(8, std::vector<uint16_t>(1024, 0));
    for (size_t i = 0; i < nsample; ++i) {
      const auto dummy = fread(packed_sample_frame.data(), sizeof(uint32_t), 3, file_.get());
      size_t ich = 0;
      for (const auto& sample : wordsUnpacker(packed_sample_frame))
        channel_samples.at(ich++).at(i) = sample;
    }

    // Trigger channel
    if (group_info.triggerChannel()) {
      auto& trigger_samples = channel_samples.emplace_back(std::vector<uint16_t>(1024, 0));
      for (size_t i = 0; i < nsample / 8; ++i) {
        const auto dummy = fread(packed_sample_frame.data(), sizeof(uint32_t), 3, file_.get());
        size_t ismp = 0;
        for (const auto& sample : wordsUnpacker(packed_sample_frame))
          trigger_samples.at(i * 8 + ismp) = sample;
      }
    }

    //************************************
    // Loop over channels 0-8
    //************************************

    for (size_t i = 0; i < channel_samples.size(); ++i) {
      const auto& channel_calibrations = calibrations_.groupCalibrations(group).channelCalibrations(i);
      const auto& off_mean = channel_calibrations.offMean();
      const auto& calib_sample = channel_calibrations.calibSample();
      // Fill pulses
      const auto& channel_raw_waveform = channel_samples.at(i);
      Waveform channel_waveform(channel_raw_waveform.size());
      for (size_t j = 0; j < channel_raw_waveform.size(); ++j)
        channel_waveform.at(j) =
            1000. * (((double(channel_raw_waveform.at(j)) - double(off_mean.at((j + tcn) % 1024))) -
                      double(calib_sample.at(j))) /
                         4095. -
                     0.5);
      group_info.addChannelWaveform(i, channel_waveform);
    }
    {  // Read group trailer (unused)
      uint32_t trailer_payload;
      const auto dummy = fread(&trailer_payload, sizeof(uint32_t), 1, file_.get());
    }
  }
  return true;
}

std::vector<uint16_t> Reader::ModuleFileReader::wordsUnpacker(const std::array<uint32_t, 3>& words) {
  return std::vector<uint16_t>{uint16_t(words.at(0) & 0xfff),
                               uint16_t((words.at(0) >> 12) & 0xfff),
                               uint16_t((words.at(0) >> 24) | ((words.at(1) & 0xf) << 8)),
                               uint16_t((words.at(1) >> 4) & 0xfff),
                               uint16_t((words.at(1) >> 16) & 0xfff),
                               uint16_t((words.at(1) >> 28) | ((words.at(2) & 0xff) << 4)),
                               uint16_t((words.at(2) >> 8) & 0xfff),
                               uint16_t(words.at(2) >> 20)};
}
