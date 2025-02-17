/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define LOG_TAG "android.hardware.health@2.1-impl-crosshatch"
#include <android-base/logging.h>

#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <android/hardware/health/2.0/types.h>
#include <health2impl/Health.h>
#include <health/utils.h>
#include <hal_conversion.h>

#include <pixelhealth/BatteryDefender.h>
#include <pixelhealth/BatteryMetricsLogger.h>
#include <pixelhealth/BatteryThermalControl.h>
#include <pixelhealth/CycleCountBackupRestore.h>
#include <pixelhealth/DeviceHealth.h>
#include <pixelhealth/LowBatteryShutdownMetrics.h>

#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

#include "BatteryRechargingControl.h"

namespace {

using namespace std::literals;

using android::hardware::health::V1_0::hal_conversion::convertFromHealthInfo;
using android::hardware::health::V1_0::hal_conversion::convertToHealthInfo;
using android::hardware::health::V2_0::DiskStats;
using android::hardware::health::V2_0::StorageAttribute;
using android::hardware::health::V2_0::StorageInfo;
using android::hardware::health::V2_0::Result;
using ::android::hardware::health::V2_1::IHealth;
using android::hardware::health::InitHealthdConfig;
using ::device::google::crosshatch::health::BatteryRechargingControl;

using hardware::google::pixel::health::BatteryDefender;
using hardware::google::pixel::health::BatteryMetricsLogger;
using hardware::google::pixel::health::BatteryThermalControl;
using hardware::google::pixel::health::CycleCountBackupRestore;
using hardware::google::pixel::health::DeviceHealth;
using hardware::google::pixel::health::LowBatteryShutdownMetrics;

constexpr char kBatteryResistance[] = "/sys/class/power_supply/maxfg/resistance";
constexpr char kBatteryOCV[] = "/sys/class/power_supply/maxfg/voltage_ocv";
constexpr char kVoltageAvg[] = "/sys/class/power_supply/maxfg/voltage_avg";

#define WLC_DIR "/sys/class/power_supply/wireless"

static BatteryDefender battDefender(WLC_DIR "/online");
static BatteryRechargingControl battRechargingControl;
static BatteryThermalControl battThermalControl("sys/devices/virtual/thermal/tz-by-name/soc/mode");
static BatteryMetricsLogger battMetricsLogger(kBatteryResistance, kBatteryOCV);
static LowBatteryShutdownMetrics shutdownMetrics(kVoltageAvg);
static CycleCountBackupRestore ccBackupRestoreBMS(
    8, "/sys/class/power_supply/bms/device/cycle_counts_bins",
    "/mnt/vendor/persist/battery/qcom_cycle_counts_bins");
static CycleCountBackupRestore ccBackupRestoreMAX(
    10, "/sys/class/power_supply/maxfg/cycle_counts_bins",
    "/mnt/vendor/persist/battery/max_cycle_counts_bins",
    "/sys/class/power_supply/maxfg/serial_number");
static DeviceHealth deviceHealth;

#define UFS_DIR "/sys/devices/platform/soc/1d84000.ufshc"
const std::string kUfsHealthEol{UFS_DIR "/health/eol"};
const std::string kUfsHealthLifetimeA{UFS_DIR "/health/lifetimeA"};
const std::string kUfsHealthLifetimeB{UFS_DIR "/health/lifetimeB"};
const std::string kUfsVersion{UFS_DIR "/version"};
const std::string kDiskStatsFile{"/sys/block/sda/stat"};
const std::string kUFSName{"UFS0"};

static bool needs_wlc_updates = false;
constexpr char kWlcCapacity[]{WLC_DIR "/capacity"};

std::ifstream assert_open(const std::string &path) {
    std::ifstream stream(path);
    if (!stream.is_open()) {
        LOG(WARNING) << "Cannot read " << path;
    }
    return stream;
}

template <typename T>
void read_value_from_file(const std::string &path, T *field) {
    auto stream = assert_open(path);
    stream.unsetf(std::ios_base::basefield);
    stream >> *field;
}

void read_ufs_version(StorageInfo *info) {
    uint64_t value;
    read_value_from_file(kUfsVersion, &value);
    std::stringstream ss;
    ss << "ufs " << std::hex << value;
    info->version = ss.str();
}

void fill_ufs_storage_attribute(StorageAttribute *attr) {
    attr->isInternal = true;
    attr->isBootDevice = true;
    attr->name = kUFSName;
}

static bool FileExists(const std::string &filename) {
    struct stat buffer;

    return stat(filename.c_str(), &buffer) == 0;
}

void private_healthd_board_init(struct healthd_config *config) {
    using ::device::google::crosshatch::health::kChargerStatus;

    ccBackupRestoreBMS.Restore();
    ccBackupRestoreMAX.Restore();

    config->batteryStatusPath = kChargerStatus.c_str();

    needs_wlc_updates = FileExists(kWlcCapacity);
}

int private_healthd_board_battery_update(struct android::BatteryProperties *props) {
    battRechargingControl.updateBatteryProperties(props);
    deviceHealth.update(props);
    battThermalControl.updateThermalState(props);
    battMetricsLogger.logBatteryProperties(props);
    shutdownMetrics.logShutdownVoltage(props);
    ccBackupRestoreBMS.Backup(props->batteryLevel);
    ccBackupRestoreMAX.Backup(props->batteryLevel);
    battDefender.update(props);

    if (needs_wlc_updates &&
        !android::base::WriteStringToFile(std::to_string(props->batteryLevel), kWlcCapacity))
        LOG(INFO) << "Unable to write battery level to wireless capacity";

    return 0;
}

void private_get_storage_info(std::vector<StorageInfo> &vec_storage_info) {
    vec_storage_info.resize(1);
    StorageInfo *storage_info = &vec_storage_info[0];
    fill_ufs_storage_attribute(&storage_info->attr);

    read_ufs_version(storage_info);
    read_value_from_file(kUfsHealthEol, &storage_info->eol);
    read_value_from_file(kUfsHealthLifetimeA, &storage_info->lifetimeA);
    read_value_from_file(kUfsHealthLifetimeB, &storage_info->lifetimeB);
    return;
}

void private_get_disk_stats(std::vector<DiskStats> &vec_stats) {
    vec_stats.resize(1);
    DiskStats *stats = &vec_stats[0];
    fill_ufs_storage_attribute(&stats->attr);

    auto stream = assert_open(kDiskStatsFile);
    // Regular diskstats entries
    stream >> stats->reads >> stats->readMerges >> stats->readSectors >> stats->readTicks >>
        stats->writes >> stats->writeMerges >> stats->writeSectors >> stats->writeTicks >>
        stats->ioInFlight >> stats->ioTicks >> stats->ioInQueue;
    return;
}
}  // anonymous namespace

namespace android {
namespace hardware {
namespace health {
namespace V2_1 {
namespace implementation {
class HealthImpl : public Health {
 public:
  HealthImpl(std::unique_ptr<healthd_config>&& config)
    : Health(std::move(config)) {}

  Return<void> getStorageInfo(getStorageInfo_cb _hidl_cb) override;
  Return<void> getDiskStats(getDiskStats_cb _hidl_cb) override;

 protected:
  void UpdateHealthInfo(HealthInfo* health_info) override;

};

void HealthImpl::UpdateHealthInfo(HealthInfo* health_info) {
  struct BatteryProperties props;
  convertFromHealthInfo(health_info->legacy.legacy, &props);
  private_healthd_board_battery_update(&props);
  convertToHealthInfo(&props, health_info->legacy.legacy);
}

Return<void> HealthImpl::getStorageInfo(getStorageInfo_cb _hidl_cb)
{
  std::vector<struct StorageInfo> info;
  private_get_storage_info(info);
  hidl_vec<struct StorageInfo> info_vec(info);
  if (!info.size()) {
      _hidl_cb(Result::NOT_SUPPORTED, info_vec);
  } else {
      _hidl_cb(Result::SUCCESS, info_vec);
  }
  return Void();
}

Return<void> HealthImpl::getDiskStats(getDiskStats_cb _hidl_cb)
{
  std::vector<struct DiskStats> stats;
  private_get_disk_stats(stats);
  hidl_vec<struct DiskStats> stats_vec(stats);
  if (!stats.size()) {
      _hidl_cb(Result::NOT_SUPPORTED, stats_vec);
  } else {
      _hidl_cb(Result::SUCCESS, stats_vec);
  }
  return Void();
}

}  // namespace implementation
}  // namespace V2_1
}  // namespace health
}  // namespace hardware
}  // namespace android

extern "C" IHealth* HIDL_FETCH_IHealth(const char* instance) {
  using ::android::hardware::health::V2_1::implementation::HealthImpl;
  if (instance != "default"sv) {
      return nullptr;
  }
  auto config = std::make_unique<healthd_config>();
  InitHealthdConfig(config.get());

  private_healthd_board_init(config.get());

  return new HealthImpl(std::move(config));
}
