/*
 * Copyright 2018 The Android Open Source Project
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

#define LOG_TAG "dumpstate"

#include "DumpstateDevice.h"

#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <cutils/properties.h>
#include <hidl/HidlBinderSupport.h>
#include <hidl/HidlSupport.h>

#include <log/log.h>
#include <pthread.h>
#include <string.h>

#include <pcap.h>

#define _SVID_SOURCE
#include <dirent.h>

#include "DumpstateUtil.h"

#define MODEM_LOG_PREFIX_PROPERTY "ro.vendor.radio.log_prefix"
#define MODEM_LOG_LOC_PROPERTY "ro.vendor.radio.log_loc"
#define MODEM_LOGGING_SWITCH "persist.vendor.radio.smlog_switch"

#define DIAG_MDLOG_PERSIST_PROPERTY "persist.vendor.sys.modem.diag.mdlog"
#define DIAG_MDLOG_PROPERTY "vendor.sys.modem.diag.mdlog"
#define DIAG_MDLOG_STATUS_PROPERTY "vendor.sys.modem.diag.mdlog_on"

#define DIAG_MDLOG_NUMBER_BUGREPORT "persist.vendor.sys.modem.diag.mdlog_br_num"

#define UFS_BOOTDEVICE "ro.boot.bootdevice"

#define TCPDUMP_NUMBER_BUGREPORT "persist.vendor.tcpdump.log.br_num"
#define TCPDUMP_PERSIST_PROPERTY "persist.vendor.tcpdump.log.alwayson"

#define MODEM_EFS_DUMP_PROPERTY "vendor.sys.modem.diag.efsdump"

#define VENDOR_VERBOSE_LOGGING_ENABLED_PROPERTY "persist.vendor.verbose_logging_enabled"

#define TIME_BUFFER 24
#define STRING_BUFFER 550
#define MAX_PACKET_LENGTH 150

using android::os::dumpstate::CommandOptions;
using android::os::dumpstate::DumpFileToFd;
using android::os::dumpstate::PropertiesHelper;
using android::os::dumpstate::RunCommandToFd;

namespace android {
namespace hardware {
namespace dumpstate {
namespace V1_1 {
namespace implementation {

#define DIAG_LOG_PREFIX "diag_log_"
#define TCPDUMP_LOG_PREFIX "tcpdump"
#define EXTENDED_LOG_PREFIX "extended_log_"

static void dumpLogs(int fd, std::string srcDir, std::string destDir,
                     int maxFileNum, const char *logPrefix) {
    struct dirent **dirent_list = NULL;
    int num_entries = scandir(srcDir.c_str(),
                              &dirent_list,
                              0,
                              (int (*)(const struct dirent **, const struct dirent **)) alphasort);
    if (!dirent_list) {
        return;
    } else if (num_entries <= 0) {
        return;
    }

    int copiedFiles = 0;

    for (int i = num_entries - 1; i >= 0; i--) {
        ALOGD("Found %s\n", dirent_list[i]->d_name);

        if (0 != strncmp(dirent_list[i]->d_name, logPrefix, strlen(logPrefix))) {
            continue;
        }

        if ((copiedFiles >= maxFileNum) && (maxFileNum != -1)) {
            ALOGD("Skipped %s\n", dirent_list[i]->d_name);
            continue;
        }

        copiedFiles++;

        CommandOptions options = CommandOptions::WithTimeout(120).Build();
        std::string srcLogFile = srcDir + "/" + dirent_list[i]->d_name;
        std::string destLogFile = destDir + "/" + dirent_list[i]->d_name;

        std::string copyCmd = "/vendor/bin/cp " + srcLogFile + " " + destLogFile;

        ALOGD("Copying %s to %s\n", srcLogFile.c_str(), destLogFile.c_str());
        RunCommandToFd(fd, "CP DIAG LOGS", { "/vendor/bin/sh", "-c", copyCmd.c_str() }, options);
    }

    while (num_entries--) {
        free(dirent_list[num_entries]);
    }

    free(dirent_list);
}

// Get timestamp and packet data
static void GetTimeStampAndPkt(char *pktbuf, unsigned char *packet, char *strtime, int totalbyte){
    int idxLog = 0;
    int idxPkt = 0;
    int firstPktDataIdx = 2;
    int lastPktDataIdx = totalbyte - 2;
    char arrTmpTime[19];
    char arrStrTime[TIME_BUFFER];
    char* tmpPktByteStr;
    char* pktDataStr = pktbuf;
    unsigned char arrPktData[MAX_PACKET_LENGTH];

    memset(arrTmpTime, 0, sizeof(arrTmpTime));
    while ((tmpPktByteStr = strtok_r(pktDataStr, " ", &pktDataStr))) {
      if(idxLog == 0) {
        snprintf(arrTmpTime, sizeof(arrTmpTime),"%s", tmpPktByteStr);
      }
      if(idxLog == 1) {
        snprintf(arrStrTime, sizeof(arrStrTime),"%s %s",arrTmpTime, tmpPktByteStr);
      }
      if(idxLog > firstPktDataIdx && idxLog <= lastPktDataIdx) {
        int data;
        unsigned char pktdata;
        sscanf(tmpPktByteStr,"%02x", &data);
        pktdata = (unsigned char)data;
        arrPktData[idxPkt] = pktdata;
        idxPkt++;
      }
      idxLog++;
    }
    memcpy(packet, arrPktData, MAX_PACKET_LENGTH);
    memcpy(strtime, arrStrTime, TIME_BUFFER);
}

static void ProcessPcapDump(FILE *fp, pcap_dumper_t *dumper)
{
    ALOGD("ProcessPcapDump(): enter");
    char strLogBuf[STRING_BUFFER];
    char arrPktBuf[STRING_BUFFER];
    char arrPktSplitBuf[STRING_BUFFER];
    char arrStrTime[TIME_BUFFER];
    unsigned char arrPktData[MAX_PACKET_LENGTH];

    if(fp  == NULL) {
      ALOGD("can not read extended_log_datastall file!");
      return;
    }
    if(dumper == NULL) {
      ALOGD("can not open pcap file.");
      return;
    }

    while (!feof(fp)) {
      while (fgets(strLogBuf,STRING_BUFFER,fp)) {
        if(strLogBuf[0] == '\n') {
          continue;
        }
        memcpy(arrPktBuf, strLogBuf, sizeof(arrPktBuf));
        memcpy(arrPktSplitBuf, strLogBuf, sizeof(arrPktSplitBuf));

        int countPktLen = 0;
        char* tmpPktByteStr;
        char* pktDataStr = arrPktSplitBuf;
        while ((tmpPktByteStr = strtok_r(pktDataStr, " ", &pktDataStr))) {
          countPktLen+=1;
        }

        // Get timestamp and packet data
        GetTimeStampAndPkt(arrPktBuf ,arrPktData, arrStrTime, countPktLen);

        // Build packet header
        int timeMSec;
        char* strTime;
        char* strTimeMsec;
        char* strTmpTime = arrStrTime;
        struct pcap_pkthdr pcap_hdr;
        while ((strTime = strtok_r(strTmpTime, ".", &strTmpTime))) {
          if(strTmpTime == NULL) {
            break;
          }
          time_t time;
          struct tm timeStruct;
          memset(&timeStruct, 0, sizeof(struct tm));
          if(strlen(strTime) == 19) {
            strptime(strTime, "%Y-%m-%d %H:%M:%S", &timeStruct);
            time = mktime(&timeStruct);
            pcap_hdr.ts.tv_sec = time;
          }
          strTimeMsec = strtok_r(strTmpTime, ".", &strTmpTime);
          if(strTimeMsec == NULL) {
            break;
          }
          timeMSec = atoi(strTimeMsec);
          pcap_hdr.ts.tv_usec = timeMSec;
        }
        pcap_hdr.caplen = sizeof(arrPktData);
        pcap_hdr.len = pcap_hdr.caplen;
        pcap_dump((u_char *)dumper, &pcap_hdr, arrPktData);
      }
    }
}

static void MergeAndConvertToPcap(char* logFile, char* oldlogFile, char* pcapFile) {

    ALOGD("DumpPcap(): enter");
    pcap_t *handle = pcap_open_dead(DLT_EN10MB, 1 << 16);
    pcap_dumper_t *dumper = pcap_dump_open(handle, pcapFile);

    if(dumper == NULL) {
      ALOGD("can not open pcap file.");
      return;
    }

    FILE *fp = fopen(oldlogFile, "r");
    if(fp != NULL) {
      ProcessPcapDump(fp, dumper);
      fclose(fp);
    }
    fp = fopen(logFile, "r");
    if(fp != NULL) {
      ProcessPcapDump(fp, dumper);
      fclose(fp);
    }
    pcap_dump_close(dumper);
}

static void *dumpModemThread(void *data)
{
    long fdModem = (long)data;

    ALOGD("dumpModemThread started\n");

    std::string modemLogDir = android::base::GetProperty(MODEM_LOG_LOC_PROPERTY, "");
    if (modemLogDir.empty()) {
        ALOGD("No modem log place is set");
        return NULL;
    }

    std::string filePrefix = android::base::GetProperty(MODEM_LOG_PREFIX_PROPERTY, "");

    if (filePrefix.empty()) {
        ALOGD("Modem log prefix is not set");
        return NULL;
    }

    sleep(1);
    ALOGD("Waited modem for 1 second to flush logs");

    const std::string modemLogCombined = modemLogDir + "/" + filePrefix + "all.tar";
    const std::string modemLogAllDir = modemLogDir + "/modem_log";

    RunCommandToFd(STDOUT_FILENO, "MKDIR MODEM LOG", {"/vendor/bin/mkdir", "-p", modemLogAllDir.c_str()}, CommandOptions::WithTimeout(2).Build());

    const std::string diagLogDir = "/data/vendor/radio/diag_logs/logs";
    const std::string diagPoweronLogPath = "/data/vendor/radio/diag_logs/logs/diag_poweron_log.qmdl";

    bool diagLogEnabled = android::base::GetBoolProperty(DIAG_MDLOG_PERSIST_PROPERTY, false);

    if (diagLogEnabled) {
        bool diagLogStarted = android::base::GetBoolProperty( DIAG_MDLOG_STATUS_PROPERTY, false);

        if (diagLogStarted) {
            android::base::SetProperty(DIAG_MDLOG_PROPERTY, "false");
            ALOGD("Stopping diag_mdlog...\n");
            if (android::base::WaitForProperty(DIAG_MDLOG_STATUS_PROPERTY, "false", std::chrono::seconds(10))) {
                ALOGD("diag_mdlog exited");
            } else {
                ALOGE("Waited mdlog timeout after 10 second");
            }
        } else {
            ALOGD("diag_mdlog is not running");
        }

        dumpLogs(STDOUT_FILENO, diagLogDir, modemLogAllDir, android::base::GetIntProperty(DIAG_MDLOG_NUMBER_BUGREPORT, 100), DIAG_LOG_PREFIX);

        if (diagLogStarted) {
            ALOGD("Restarting diag_mdlog...");
            android::base::SetProperty(DIAG_MDLOG_PROPERTY, "true");
        }
    }
    RunCommandToFd(STDOUT_FILENO, "CP MODEM POWERON LOG", {"/vendor/bin/cp", diagPoweronLogPath.c_str(), modemLogAllDir.c_str()}, CommandOptions::WithTimeout(2).Build());

    // dump to pcap
    char fpcapname[]="/data/vendor/radio/extended_logs/extended_log_datastall.pcap";
    char flogname[]="/data/vendor/radio/extended_logs/extended_log_datastall.txt";
    char flogoldname[]="/data/vendor/radio/extended_logs/extended_log_datastall.txt.old";
    MergeAndConvertToPcap(flogname, flogoldname, fpcapname);

    if (!PropertiesHelper::IsUserBuild()) {
        char cmd[256] = { 0 };

        android::base::SetProperty(MODEM_EFS_DUMP_PROPERTY, "true");

        const std::string tcpdumpLogDir = "/data/vendor/tcpdump_logger/logs";
        const std::string extendedLogDir = "/data/vendor/radio/extended_logs";
        const std::vector <std::string> rilAndNetmgrLogs
            {
                "/data/vendor/radio/haldebug_ril0",
                "/data/vendor/radio/haldebug_ril1",
                "/data/vendor/radio/ril_log0",
                "/data/vendor/radio/ril_log0_old",
                "/data/vendor/radio/ril_log1",
                "/data/vendor/radio/ril_log1_old",
                "/data/vendor/radio/qmi_fw_log",
                "/data/vendor/radio/qmi_fw_log_old",
                "/data/vendor/radio/imsdatadaemon_log",
                "/data/vendor/radio/imsdatadaemon_log_old",
                "/data/vendor/netmgr/netmgr_log",
                "/data/vendor/netmgr/netmgr_log_old",
                "/data/vendor/radio/omadm_logs.txt",
                "/data/vendor/radio/power_anomaly_data.txt",
                "/data/vendor/radio/diag_logs/diag_trace.txt",
                "/data/vendor/radio/diag_logs/diag_trace_old.txt",
                "/data/vendor/radio/metrics_data",
                "/data/vendor/ssrlog/ssr_log.txt",
                "/data/vendor/ssrlog/ssr_log_old.txt",
                "/data/vendor/rfs/mpss/modem_efs",
                "/sys/kernel/debug/ipa/ipa_statistics_msg"
            };

        bool tcpdumpEnabled = android::base::GetBoolProperty(TCPDUMP_PERSIST_PROPERTY, false);
        if (tcpdumpEnabled) {
            dumpLogs(STDOUT_FILENO, tcpdumpLogDir, modemLogAllDir, android::base::GetIntProperty(TCPDUMP_NUMBER_BUGREPORT, 5), TCPDUMP_LOG_PREFIX);
        }

        for (const auto& logFile : rilAndNetmgrLogs) {
            RunCommandToFd(STDOUT_FILENO, "CP MODEM LOG", {"/vendor/bin/cp", logFile.c_str(), modemLogAllDir.c_str()}, CommandOptions::WithTimeout(2).Build());
        }

        //Dump IPA log
        snprintf(cmd, sizeof(cmd),
                "cat /d/ipc_logging/ipa/log > %s/ipa_log",
                modemLogAllDir.c_str());
        RunCommandToFd(STDOUT_FILENO, "Dump IPA log", {"/vendor/bin/sh", "-c", cmd});

        dumpLogs(STDOUT_FILENO, extendedLogDir, modemLogAllDir, 100, EXTENDED_LOG_PREFIX);
        android::base::SetProperty(MODEM_EFS_DUMP_PROPERTY, "false");
    }

    RunCommandToFd(STDOUT_FILENO, "TAR LOG", {"/vendor/bin/tar", "cvf", modemLogCombined.c_str(), "-C", modemLogAllDir.c_str(), "."}, CommandOptions::WithTimeout(20).Build());
    RunCommandToFd(STDOUT_FILENO, "CHG PERM", {"/vendor/bin/chmod", "a+w", modemLogCombined.c_str()}, CommandOptions::WithTimeout(2).Build());

    std::vector<uint8_t> buffer(65536);
    android::base::unique_fd fdLog(TEMP_FAILURE_RETRY(open(modemLogCombined.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)));

    if (fdLog >= 0) {
        while (1) {
            ssize_t bytes_read = TEMP_FAILURE_RETRY(read(fdLog, buffer.data(), buffer.size()));

            if (bytes_read == 0) {
                break;
            } else if (bytes_read < 0) {
                ALOGD("read(%s): %s\n", modemLogCombined.c_str(), strerror(errno));
                break;
            }

            ssize_t result = TEMP_FAILURE_RETRY(write(fdModem, buffer.data(), bytes_read));

            if (result != bytes_read) {
                ALOGD("Failed to write %ld bytes, actually written: %ld", bytes_read, result);
                break;
            }
        }
    }

    RunCommandToFd(STDOUT_FILENO, "RM MODEM DIR", { "/vendor/bin/rm", "-r", modemLogAllDir.c_str()}, CommandOptions::WithTimeout(2).Build());
    RunCommandToFd(STDOUT_FILENO, "RM LOG", { "/vendor/bin/rm", modemLogCombined.c_str()}, CommandOptions::WithTimeout(2).Build());

    ALOGD("dumpModemThread finished\n");

    return NULL;
}

static void DumpTouch(int fd) {
    const char touch_spi_path[] = "/sys/devices/virtual/sec/tsp";
    char cmd[256];

    if (!access(touch_spi_path, R_OK)) {
        //Enable: force touch active
        snprintf(cmd, sizeof(cmd),
                 "echo %s > %s/cmd && cat %s/cmd_result",
                 "force_touch_active,1",
                 touch_spi_path, touch_spi_path);
        RunCommandToFd(fd, "Force Touch Active", {"/vendor/bin/sh", "-c", cmd});

        //Change data format from portrait to landscape
        snprintf(cmd, sizeof(cmd),
                 "echo %s > %s/cmd && cat %s/cmd_result",
                 "set_print_format,1",
                 touch_spi_path, touch_spi_path);
        RunCommandToFd(fd, "Print Format", {"/vendor/bin/sh", "-c", cmd});

        //Firmware info
        snprintf(cmd, sizeof(cmd), "%s/fw_version", touch_spi_path);
        DumpFileToFd(fd, "LSI firmware version", cmd);

        //Touch status
        snprintf(cmd, sizeof(cmd), "%s/status", touch_spi_path);
        DumpFileToFd(fd, "LSI touch status", cmd);

        //Calibration info
        snprintf(cmd, sizeof(cmd),
                 "echo %s > %s/cmd && cat %s/cmd_result",
                 "get_mis_cal_info",
                 touch_spi_path, touch_spi_path);
        RunCommandToFd(fd, "Calibration info", {"/vendor/bin/sh", "-c", cmd});

        //Mutual strength
        snprintf(cmd, sizeof(cmd),
                 "echo %s > %s/cmd && cat %s/cmd_result",
                 "run_delta_read_all",
                 touch_spi_path, touch_spi_path);
        RunCommandToFd(fd, "Mutual Strength", {"/vendor/bin/sh", "-c", cmd});

        //Self strength
        snprintf(cmd, sizeof(cmd),
                 "echo %s > %s/cmd && cat %s/cmd_result",
                 "run_self_delta_read_all",
                 touch_spi_path, touch_spi_path);
        RunCommandToFd(fd, "Self Strength", {"/vendor/bin/sh", "-c", cmd});

        //Raw cap
        snprintf(cmd, sizeof(cmd),
                 "echo %s > %s/cmd && cat %s/cmd_result",
                 "run_rawcap_read_all",
                 touch_spi_path, touch_spi_path);
        RunCommandToFd(fd, "Mutual Raw Cap", {"/vendor/bin/sh", "-c", cmd});

        //Self raw cap
        snprintf(cmd, sizeof(cmd),
                 "echo %s > %s/cmd && cat %s/cmd_result",
                 "run_self_rawcap_read_all",
                 touch_spi_path, touch_spi_path);
        RunCommandToFd(fd, "Self Raw Cap", {"/vendor/bin/sh", "-c", cmd});

        //TYPE_AMBIENT_DATA
        snprintf(cmd, sizeof(cmd),
                 "echo %s > %s/cmd && cat %s/cmd_result",
                 "run_rawdata_read_type,3",
                 touch_spi_path, touch_spi_path);
        RunCommandToFd(fd, "TYPE_AMBIENT_DATA", {"/vendor/bin/sh", "-c", cmd});

        //TYPE_DECODED_DATA
        snprintf(cmd, sizeof(cmd),
                 "echo %s > %s/cmd && cat %s/cmd_result",
                 "run_rawdata_read_type,5",
                 touch_spi_path, touch_spi_path);
        RunCommandToFd(fd, "TYPE_DECODED_DATA", {"/vendor/bin/sh", "-c", cmd});

        //TYPE_NOI_P2P_MIN
        snprintf(cmd, sizeof(cmd),
                 "echo %s > %s/cmd && cat %s/cmd_result",
                 "run_rawdata_read_type,30",
                 touch_spi_path, touch_spi_path);
        RunCommandToFd(fd, "TYPE_NOI_P2P_MIN", {"/vendor/bin/sh", "-c", cmd});

        //TYPE_NOI_P2P_MAX
        snprintf(cmd, sizeof(cmd),
                 "echo %s > %s/cmd && cat %s/cmd_result",
                 "run_rawdata_read_type,31",
                 touch_spi_path, touch_spi_path);
        RunCommandToFd(fd, "TYPE_NOI_P2P_MAX", {"/vendor/bin/sh", "-c", cmd});

        //Change data format back to default(portrait)
        snprintf(cmd, sizeof(cmd),
                 "echo %s > %s/cmd && cat %s/cmd_result",
                 "set_print_format,0",
                 touch_spi_path, touch_spi_path);
        RunCommandToFd(fd, "Print Format", {"/vendor/bin/sh", "-c", cmd});


        //Disable: force touch active
        snprintf(cmd, sizeof(cmd),
                 "echo %s > %s/cmd && cat %s/cmd_result",
                 "force_touch_active,0",
                 touch_spi_path, touch_spi_path);
        RunCommandToFd(fd, "Force Touch Active", {"/vendor/bin/sh", "-c", cmd});
    }

}

static void DumpDisplay(int fd) {
    DumpFileToFd(fd, "PANEL VENDOR NAME", "/sys/class/panel_info/panel0/panel_vendor_name");
    DumpFileToFd(fd, "PANEL SN", "/sys/class/panel_info/panel0/serial_number");
    DumpFileToFd(fd, "PANEL EXTRA INFO", "/sys/class/panel_info/panel0/panel_extinfo");
}

static void DumpSensorLog(int fd) {
    const std::string logPath = "/data/vendor/sensors/log/sensor_log.txt";
    const std::string lastlogPath = "/data/vendor/sensors/log/sensor_lastlog.txt";

    if (!access(logPath.c_str(), R_OK)) {
        DumpFileToFd(fd, "sensor log", logPath);
    }
    if (!access(lastlogPath.c_str(), R_OK)) {
        DumpFileToFd(fd, "sensor lastlog", lastlogPath);
    }
}

static void DumpF2FS(int fd) {
    DumpFileToFd(fd, "F2FS", "/sys/kernel/debug/f2fs/status");
    RunCommandToFd(fd, "F2FS - fragmentation", {"/vendor/bin/sh", "-c",
                       "for d in $(ls /proc/fs/f2fs/); do "
                       "echo $d: /dev/block/mapper/`ls -l /dev/block/mapper | grep $d | awk '{print $8,$9,$10}'`; "
                       "cat /proc/fs/f2fs/$d/segment_info; done"});
    RunCommandToFd(fd, "F2FS - fsck time (ms)", {"/vendor/bin/sh", "-c", "getprop ro.boottime.init.fsck.data"});
    RunCommandToFd(fd, "F2FS - checkpoint=disable time (ms)", {"/vendor/bin/sh", "-c", "getprop ro.boottime.init.mount.data"});
}

static void DumpUFS(int fd) {
    DumpFileToFd(fd, "UFS model", "/sys/block/sda/device/model");
    DumpFileToFd(fd, "UFS rev", "/sys/block/sda/device/rev");
    DumpFileToFd(fd, "UFS size", "/sys/block/sda/size");

    std::string bootdev = android::base::GetProperty(UFS_BOOTDEVICE, "");
    if (!bootdev.empty()) {
        DumpFileToFd(fd, "UFS Slow IO Read", "/sys/devices/platform/soc/" + bootdev + "/slowio_read_cnt");
        DumpFileToFd(fd, "UFS Slow IO Write", "/sys/devices/platform/soc/" + bootdev + "/slowio_write_cnt");
        DumpFileToFd(fd, "UFS Slow IO Unmap", "/sys/devices/platform/soc/" + bootdev + "/slowio_unmap_cnt");
        DumpFileToFd(fd, "UFS Slow IO Sync", "/sys/devices/platform/soc/" + bootdev + "/slowio_sync_cnt");

        RunCommandToFd(fd, "UFS err_stats", {"/vendor/bin/sh", "-c",
                           "path=\"/sys/devices/platform/soc/" + bootdev + "/err_stats\"; "
                           "for node in `ls $path/err_*`; do "
                           "printf \"%s:%d\\n\" $(basename $node) $(cat $node); done;"});

        RunCommandToFd(fd, "UFS io_stats", {"/vendor/bin/sh", "-c",
                           "path=\"/sys/devices/platform/soc/" + bootdev + "/io_stats\"; "
                           "printf \"\\t\\t%-10s %-10s %-10s %-10s %-10s %-10s\\n\" "
                           "ReadCnt ReadBytes WriteCnt WriteBytes RWCnt RWBytes; "
                           "str=$(cat $path/*_start); arr=($str); "
                           "printf \"Started: \\t%-10s %-10s %-10s %-10s %-10s %-10s\\n\" "
                           "${arr[1]} ${arr[0]} ${arr[5]} ${arr[4]} ${arr[3]} ${arr[2]}; "
                           "str=$(cat $path/*_complete); arr=($str); "
                           "printf \"Completed: \\t%-10s %-10s %-10s %-10s %-10s %-10s\\n\" "
                           "${arr[1]} ${arr[0]} ${arr[5]} ${arr[4]} ${arr[3]} ${arr[2]}; "
                           "str=$(cat $path/*_maxdiff); arr=($str); "
                           "printf \"MaxDiff: \\t%-10s %-10s %-10s %-10s %-10s %-10s\\n\\n\" "
                           "${arr[1]} ${arr[0]} ${arr[5]} ${arr[4]} ${arr[3]} ${arr[2]}; "});

        RunCommandToFd(fd, "UFS req_stats", {"/vendor/bin/sh", "-c",
                           "path=\"/sys/devices/platform/soc/" + bootdev + "/req_stats\"; "
                           "printf \"\\t%-10s %-10s %-10s %-10s %-10s %-10s %-10s\\n\" "
                           "All Write Read Read\\(urg\\) Write\\(urg\\) Flush Discard; "
                           "str=$(cat $path/*_min); arr=($str); "
                           "printf \"Min:\\t%-10s %-10s %-10s %-10s %-10s %-10s %-10s\\n\" "
                           "${arr[0]} ${arr[3]} ${arr[6]} ${arr[4]} ${arr[5]} ${arr[2]} ${arr[1]}; "
                           "str=$(cat $path/*_max); arr=($str); "
                           "printf \"Max:\\t%-10s %-10s %-10s %-10s %-10s %-10s %-10s\\n\" "
                           "${arr[0]} ${arr[3]} ${arr[6]} ${arr[4]} ${arr[5]} ${arr[2]} ${arr[1]}; "
                           "str=$(cat $path/*_avg); arr=($str); "
                           "printf \"Avg.:\\t%-10s %-10s %-10s %-10s %-10s %-10s %-10s\\n\" "
                           "${arr[0]} ${arr[3]} ${arr[6]} ${arr[4]} ${arr[5]} ${arr[2]} ${arr[1]}; "
                           "str=$(cat $path/*_sum); arr=($str); "
                           "printf \"Count:\\t%-10s %-10s %-10s %-10s %-10s %-10s %-10s\\n\\n\" "
                           "${arr[0]} ${arr[3]} ${arr[6]} ${arr[4]} ${arr[5]} ${arr[2]} ${arr[1]};"});

        std::string ufs_health = "for f in $(find /sys/devices/platform/soc/" + bootdev + "/health_descriptor -type f); do if [[ -r $f && -f $f ]]; then echo --- $f; cat $f; echo ''; fi; done";
        RunCommandToFd(fd, "UFS health", {"/vendor/bin/sh", "-c", ufs_health.c_str()});
    }
}

// Methods from ::android::hardware::dumpstate::V1_0::IDumpstateDevice follow.
Return<void> DumpstateDevice::dumpstateBoard(const hidl_handle& handle) {
    // Ignore return value, just return an empty status.
    dumpstateBoard_1_1(handle, DumpstateMode::DEFAULT, 30 * 1000 /* timeoutMillis */);
    return Void();
}

// Methods from ::android::hardware::dumpstate::V1_1::IDumpstateDevice follow.
Return<DumpstateStatus> DumpstateDevice::dumpstateBoard_1_1(const hidl_handle& handle,
                                                            const DumpstateMode mode,
                                                            const uint64_t timeoutMillis) {
    // Unused arguments.
    (void) timeoutMillis;

    // Exit when dump is completed since this is a lazy HAL.
    addPostCommandTask([]() {
        exit(0);
    });

    if (handle == nullptr || handle->numFds < 1) {
        ALOGE("no FDs\n");
        return DumpstateStatus::ILLEGAL_ARGUMENT;
    }

    int fd = handle->data[0];
    if (fd < 0) {
        ALOGE("invalid FD: %d\n", handle->data[0]);
        return DumpstateStatus::ILLEGAL_ARGUMENT;
    }

    bool isModeValid = false;
    for (const auto dumpstateMode : hidl_enum_range<DumpstateMode>()) {
        if (mode == dumpstateMode) {
            isModeValid = true;
            break;
        }
    }
    if (!isModeValid) {
        ALOGE("Invalid mode: %d\n", mode);
        return DumpstateStatus::ILLEGAL_ARGUMENT;
    } else if (mode == DumpstateMode::WEAR) {
        // We aren't a Wear device.
        ALOGE("Unsupported mode: %d\n", mode);
        return DumpstateStatus::UNSUPPORTED_MODE;
    }

    RunCommandToFd(fd, "Notify modem", {"/vendor/bin/modem_svc", "-s"}, CommandOptions::WithTimeout(1).Build());

    pthread_t modemThreadHandle = 0;
    if (getVerboseLoggingEnabled()) {
        ALOGD("Verbose logging is enabled.\n");
        if (handle->numFds < 2) {
            ALOGE("no FD for modem\n");
        } else {
            int fdModem = handle->data[1];
            if (pthread_create(&modemThreadHandle, NULL, dumpModemThread, (void *)((long)fdModem)) != 0) {
                ALOGE("could not create thread for dumpModem\n");
            }
        }
    }

    RunCommandToFd(fd, "VENDOR PROPERTIES", {"/vendor/bin/getprop"});
    DumpFileToFd(fd, "SoC serial number", "/sys/devices/soc0/serial_number");
    DumpFileToFd(fd, "CPU present", "/sys/devices/system/cpu/present");
    DumpFileToFd(fd, "CPU online", "/sys/devices/system/cpu/online");
    DumpFileToFd(fd, "Bootloader Log", "/proc/bldrlog");
    DumpTouch(fd);
    DumpDisplay(fd);

    DumpF2FS(fd);
    DumpUFS(fd);

    DumpSensorLog(fd);

    DumpFileToFd(fd, "INTERRUPTS", "/proc/interrupts");
    DumpFileToFd(fd, "Sleep Stats", "/sys/power/system_sleep/stats");
    DumpFileToFd(fd, "Power Management Stats", "/sys/power/rpmh_stats/master_stats");
    DumpFileToFd(fd, "WLAN Power Stats", "/sys/kernel/wlan/power_stats");
    DumpFileToFd(fd, "LL-Stats", "/d/wlan0/ll_stats");
    DumpFileToFd(fd, "WLAN Connect Info", "/d/wlan0/connect_info");
    DumpFileToFd(fd, "WLAN Offload Info", "/d/wlan0/offload_info");
    DumpFileToFd(fd, "WLAN Roaming Stats", "/d/wlan0/roam_stats");
    DumpFileToFd(fd, "ICNSS Stats", "/d/icnss/stats");
    DumpFileToFd(fd, "SMD Log", "/d/ipc_logging/smd/log");
    RunCommandToFd(fd, "ION HEAPS", {"/vendor/bin/sh", "-c", "for d in $(ls -d /d/ion/*); do for f in $(ls $d); do echo --- $d/$f; cat $d/$f; done; done"});
    DumpFileToFd(fd, "dmabuf info", "/d/dma_buf/bufinfo");
    DumpFileToFd(fd, "dmabuf process info", "/d/dma_buf/dmaprocs");
    RunCommandToFd(fd, "Temperatures", {"/vendor/bin/sh", "-c", "for f in /sys/class/thermal/thermal* ; do type=`cat $f/type` ; temp=`cat $f/temp` ; echo \"$type: $temp\" ; done"});
    RunCommandToFd(fd, "Cooling Device Current State", {"/vendor/bin/sh", "-c", "for f in /sys/class/thermal/cooling* ; do type=`cat $f/type` ; temp=`cat $f/cur_state` ; echo \"$type: $temp\" ; done"});
    RunCommandToFd(fd, "Cooling Device Time in State", {"/vendor/bin/sh", "-c", "for f in /sys/class/thermal/cooling* ; do type=`cat $f/type` ; temp=`cat $f/stats/time_in_state_ms` ; echo \"$type:\n$temp\" ; done"});
    RunCommandToFd(fd, "Cooling Device Trans Table", {"/vendor/bin/sh", "-c", "for f in /sys/class/thermal/cooling* ; do type=`cat $f/type` ; temp=`cat $f/stats/trans_table` ; echo \"$type:\n$temp\" ; done"});
    RunCommandToFd(
        fd, "LMH info",
        {"/vendor/bin/sh", "-c",
         "for f in /sys/bus/platform/drivers/msm_lmh_dcvs/*qcom,limits-dcvs@*/lmh_freq_limit; do "
         "state=`cat $f` ; echo \"$f: $state\" ; done"});
    RunCommandToFd(
        fd, "CPU MAX FREQ info",
        {"/vendor/bin/sh", "-c",
         "for f in /sys/devices/system/cpu/cpufreq/policy*/scaling_max_freq; do "
         "max_freq=`cat $f` ; echo \"$f: $max_freq\" ; done"});
    RunCommandToFd(fd, "CPU time-in-state", {"/vendor/bin/sh", "-c", "for cpu in /sys/devices/system/cpu/cpu*; do f=$cpu/cpufreq/stats/time_in_state; if [ ! -f $f ]; then continue; fi; echo $f:; cat $f; done"});
    RunCommandToFd(fd, "CPU cpuidle", {"/vendor/bin/sh", "-c", "for cpu in /sys/devices/system/cpu/cpu*; do for d in $cpu/cpuidle/state*; do if [ ! -d $d ]; then continue; fi; echo \"$d: `cat $d/name` `cat $d/desc` `cat $d/time` `cat $d/usage`\"; done; done"});
    RunCommandToFd(fd, "Airbrush debug info", {"/vendor/bin/sh", "-c", "for f in `ls /sys/devices/platform/soc/c84000.i2c/i2c-4/4-0066/@(*curr|temperature|vbat|total_power)`; do echo \"$f: `cat $f`\" ; done; file=/d/airbrush/airbrush_sm/chip_state; echo \"$file: `cat $file`\""});
    DumpFileToFd(fd, "TCPM logs", "/d/usb/tcpm-usbpd0");
    DumpFileToFd(fd, "PD Engine", "/dev/logbuffer_usbpd");
    DumpFileToFd(fd, "PPS", "/dev/logbuffer_pps");
    DumpFileToFd(fd, "BMS", "/dev/logbuffer_ssoc");
    DumpFileToFd(fd, "smblib", "/dev/logbuffer_smblib");
    DumpFileToFd(fd, "WLC logs", "/dev/logbuffer_wireless");
    DumpFileToFd(fd, "RTX logs", "/dev/logbuffer_rtx");
    DumpFileToFd(fd, "TTF", "/dev/logbuffer_ttf");
    DumpFileToFd(fd, "TTF details", "/sys/class/power_supply/battery/ttf_details");
    DumpFileToFd(fd, "TTF stats", "/sys/class/power_supply/battery/ttf_stats");
    DumpFileToFd(fd, "ipc-local-ports", "/d/msm_ipc_router/dump_local_ports");
    RunCommandToFd(fd, "USB Device Descriptors", {"/vendor/bin/sh", "-c", "cd /sys/bus/usb/devices/1-1 && cat product && cat bcdDevice; cat descriptors | od -t x1 -w16 -N96"});
    RunCommandToFd(fd, "Power supply properties", {"/vendor/bin/sh", "-c", "for f in `ls /sys/class/power_supply/*/uevent` ; do echo \"------ $f\\n`cat $f`\\n\" ; done"});
    RunCommandToFd(fd, "PMIC Votables", {"/vendor/bin/sh", "-c", "cat /sys/kernel/debug/pmic-votable/*/status"});

    if (!PropertiesHelper::IsUserBuild()) {
        RunCommandToFd(fd, "Google Charger", {"/vendor/bin/sh", "-c", "cd /d/google_charger/; for f in `ls pps_*` ; do echo \"$f: `cat $f`\" ; done"});
        RunCommandToFd(fd, "Google Battery", {"/vendor/bin/sh", "-c", "cd /d/google_battery/; for f in `ls ssoc_*` ; do echo \"$f: `cat $f`\" ; done"});
    }

    RunCommandToFd(fd, "Battery EEPROM", {"/vendor/bin/sh", "-c", "xxd /sys/devices/platform/soc/98c000.i2c/i2c-1/1-0050/1-00500/nvmem"});
    DumpFileToFd(fd, "WLC VER", "/sys/devices/platform/soc/98c000.i2c/i2c-1/1-003b/version");
    DumpFileToFd(fd, "WLC STATUS", "/sys/devices/platform/soc/98c000.i2c/i2c-1/1-003b/status");

    RunCommandToFd(fd, "eSIM Status", {"/vendor/bin/sh", "-c", "od -t x1 /sys/firmware/devicetree/base/chosen/cdt/cdb2/esim"});
    DumpFileToFd(fd, "Modem Stat", "/data/vendor/modem_stat/debug.txt");
    DumpFileToFd(fd, "Pixel trace", "/d/tracing/instances/pixel-trace/trace");

    // Slower dump put later in case stuck the rest of dump
    // Timeout after 3s as TZ log missing EOF
    RunCommandToFd(fd, "QSEE logs", {"/vendor/bin/sh", "-c", "/vendor/bin/timeout 3 cat /d/tzdbg/qsee_log"});

    // Citadel info
    RunCommandToFd(fd, "Citadel VERSION", {"/vendor/bin/hw/citadel_updater", "-lv"});
    RunCommandToFd(fd, "Citadel STATS", {"/vendor/bin/hw/citadel_updater", "--stats"});
    RunCommandToFd(fd, "Citadel BOARDID", {"/vendor/bin/hw/citadel_updater", "--board_id"});

    // Dump various events in WiFi data path
    DumpFileToFd(fd, "WLAN DP Trace", "/d/wlan/dpt_stats/dump_set_dpt_logs");

    // Keep this at the end as very long on not for humans
    DumpFileToFd(fd, "WLAN FW Log Symbol Table", "/vendor/firmware/Data.msc");

    // Dump camera profiler log
    RunCommandToFd(fd, "Camera Profiler Logs", {"/vendor/bin/sh", "-c", "for f in /data/vendor/camera/profiler/camx_*; do echo [$f]; cat \"$f\";done"});

    if (modemThreadHandle) {
        pthread_join(modemThreadHandle, NULL);
    }

    return DumpstateStatus::OK;
}

Return<void> DumpstateDevice::setVerboseLoggingEnabled(const bool enable) {
    android::base::SetProperty(VENDOR_VERBOSE_LOGGING_ENABLED_PROPERTY, enable ? "true" : "false");
    return Void();
}

Return<bool> DumpstateDevice::getVerboseLoggingEnabled() {
    return android::base::GetBoolProperty(VENDOR_VERBOSE_LOGGING_ENABLED_PROPERTY, false);
}

}  // namespace implementation
}  // namespace V1_1
}  // namespace dumpstate
}  // namespace hardware
}  // namespace android
