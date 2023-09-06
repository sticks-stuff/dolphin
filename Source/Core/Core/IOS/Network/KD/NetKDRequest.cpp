// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/Network/KD/NetKDRequest.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

#include "Common/BitUtils.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/HMAC.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/NandPaths.h"
#include "Common/SettingsHandler.h"

#include "Common/Random.h"
#include "Common/ScopeGuard.h"
#include "Core/CommonTitles.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/Network/KD/VFF/VFFUtil.h"
#include "Core/IOS/Network/Socket.h"
#include "Core/IOS/Uids.h"
#include "Core/System.h"
#include "Core/WC24PatchEngine.h"

namespace IOS::HLE
{
namespace
{
enum class HardwareModel : u8
{
  RVT = 0,
  RVV = 0,
  RVL = 1,
  RVD = 2,
  Unknown = 7
};

u8 GetAreaCode(std::string_view area)
{
  static constexpr std::array<std::pair<std::string_view, u8>, 13> regions{{
      {"JPN", 0},
      {"USA", 1},
      {"EUR", 2},
      {"AUS", 2},
      {"BRA", 1},
      {"TWN", 3},
      {"ROC", 3},
      {"KOR", 4},
      {"HKG", 5},
      {"ASI", 5},
      {"LTN", 1},
      {"SAF", 2},
      {"CHN", 6},
  }};

  const auto entry_pos = std::find_if(regions.cbegin(), regions.cend(),
                                      [&area](const auto& entry) { return entry.first == area; });
  if (entry_pos != regions.end())
    return entry_pos->second;

  return 7;  // Unknown
}

HardwareModel GetHardwareModel(std::string_view model)
{
  static constexpr std::array<std::pair<std::string_view, HardwareModel>, 4> models{{
      {"RVL", HardwareModel::RVL},
      {"RVT", HardwareModel::RVT},
      {"RVV", HardwareModel::RVV},
      {"RVD", HardwareModel::RVD},
  }};

  const auto entry_pos = std::find_if(models.cbegin(), models.cend(),
                                      [&model](const auto& entry) { return entry.first == model; });
  if (entry_pos != models.cend())
    return entry_pos->second;

  return HardwareModel::Unknown;
}

s32 NWC24MakeUserID(u64* nwc24_id, u32 hollywood_id, u16 id_ctr, HardwareModel hardware_model,
                    u8 area_code)
{
  static constexpr std::array<u8, 8> table2{
      0x1, 0x5, 0x0, 0x4, 0x2, 0x3, 0x6, 0x7,
  };
  static constexpr std::array<u8, 16> table1{
      0x4, 0xB, 0x7, 0x9, 0xF, 0x1, 0xD, 0x3, 0xC, 0x2, 0x6, 0xE, 0x8, 0x0, 0xA, 0x5,
  };

  constexpr auto u64_get_byte = [](u64 value, u32 shift) -> u8 { return u8(value >> (shift * 8)); };

  constexpr auto u64_insert_byte = [](u64 value, u32 shift, u8 byte) -> u64 {
    const u64 mask = 0x00000000000000FFULL << (shift * 8);
    const u64 inst = u64{byte} << (shift * 8);
    return (value & ~mask) | inst;
  };

  u64 mix_id = (u64{area_code} << 50) | (u64(hardware_model) << 47) | (u64{hollywood_id} << 15) |
               (u64{id_ctr} << 10);
  const u64 mix_id_copy1 = mix_id;

  u32 ctr = 0;
  for (ctr = 0; ctr <= 42; ctr++)
  {
    u64 value = mix_id >> (52 - ctr);
    if ((value & 1) != 0)
    {
      value = 0x0000000000000635ULL << (42 - ctr);
      mix_id ^= value;
    }
  }

  mix_id = (mix_id_copy1 | (mix_id & 0xFFFFFFFFUL)) ^ 0x0000B3B3B3B3B3B3ULL;
  mix_id = (mix_id >> 10) | ((mix_id & 0x3FF) << (11 + 32));

  for (ctr = 0; ctr <= 5; ctr++)
  {
    const u8 ret = u64_get_byte(mix_id, ctr);
    const u8 foobar = u8((u32{table1[(ret >> 4) & 0xF]} << 4) | table1[ret & 0xF]);
    mix_id = u64_insert_byte(mix_id, ctr, foobar & 0xff);
  }

  const u64 mix_id_copy2 = mix_id;

  for (ctr = 0; ctr <= 5; ctr++)
  {
    const u8 ret = u64_get_byte(mix_id_copy2, ctr);
    mix_id = u64_insert_byte(mix_id, table2[ctr], ret);
  }

  mix_id &= 0x001FFFFFFFFFFFFFULL;
  mix_id = (mix_id << 1) | ((mix_id >> 52) & 1);

  mix_id ^= 0x00005E5E5E5E5E5EULL;
  mix_id &= 0x001FFFFFFFFFFFFFULL;

  *nwc24_id = mix_id;

  if (mix_id > 9999999999999999ULL)
    return NWC24::WC24_ERR_FATAL;

  return NWC24::WC24_OK;
}
}  // Anonymous namespace

NetKDRequestDevice::NetKDRequestDevice(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name), m_config{ios.GetFS()}, m_dl_list{ios.GetFS()},
      m_send_list{ios.GetFS()}
{
  // Enable all NWC24 permissions
  m_scheduler_buffer[1] = Common::swap32(-1);

  m_work_queue.Reset("WiiConnect24 Worker", [this](AsyncTask task) {
    const IPCReply reply = task.handler();
    {
      std::lock_guard lg(m_async_reply_lock);
      m_async_replies.emplace(AsyncReply{task.request, reply.return_value});
    }
  });

  m_handle_mail = !ios.GetIOSC().IsUsingDefaultId();
  m_scheduler_work_queue.Reset("WiiConnect24 Scheduler Worker",
                               [](std::function<void()> task) { task(); });

  m_scheduler_timer_thread = std::thread([this] { SchedulerTimer(); });
}

NetKDRequestDevice::~NetKDRequestDevice()
{
  auto socket_manager = GetEmulationKernel().GetSocketManager();
  if (socket_manager)
    socket_manager->Clean();

  {
    std::lock_guard lg(m_scheduler_lock);
    if (!m_scheduler_timer_thread.joinable())
      return;

    m_shutdown_event.Set();
  }

  m_scheduler_timer_thread.join();
}

void NetKDRequestDevice::Update()
{
  {
    std::lock_guard lg(m_async_reply_lock);
    while (!m_async_replies.empty())
    {
      const auto& reply = m_async_replies.front();
      GetEmulationKernel().EnqueueIPCReply(reply.request, reply.return_value);
      m_async_replies.pop();
    }
  }
}

void NetKDRequestDevice::SchedulerTimer()
{
  u32 mail_time_state = 0;
  u32 download_time_state = 0;
  Common::SetCurrentThreadName("KD Scheduler Timer");
  while (true)
  {
    {
      std::lock_guard lg(m_scheduler_lock);
      if (m_mail_span <= mail_time_state && m_handle_mail)
      {
        m_scheduler_work_queue.EmplaceItem([this] { SchedulerWorker(SchedulerEvent::Mail); });
        INFO_LOG_FMT(IOS_WC24, "NET_KD_REQ: Dispatching Mail Task from Scheduler");
        mail_time_state = 0;
      }

      if (m_download_span <= download_time_state)
      {
        INFO_LOG_FMT(IOS_WC24, "NET_KD_REQ: Dispatching Download Task from Scheduler");
        m_scheduler_work_queue.EmplaceItem([this] { SchedulerWorker(SchedulerEvent::Download); });
        download_time_state = 0;
      }
    }

    if (m_shutdown_event.WaitFor(std::chrono::minutes{1}))
      return;

    mail_time_state++;
    download_time_state++;
  }
}

void NetKDRequestDevice::SchedulerWorker(const SchedulerEvent event)
{
  if (event == SchedulerEvent::Download)
  {
    // TODO: Implement downloader part of scheduler
    return;
  }
  else
  {
    if (!m_config.IsRegistered())
      return;

    u32 mail_flag{};
    u32 interval{};

    NWC24::ErrorCode code = KDCheckMail(&mail_flag, &interval);
    if (code != NWC24::WC24_OK)
    {
      LogError(ErrorType::CheckMail, code);
    }
  }
}

std::string NetKDRequestDevice::GetValueFromCGIResponse(const std::string& response,
                                                        const std::string& key)
{
  const std::vector<std::string> raw_fields = SplitString(response, '\n');
  for (const std::string& field : raw_fields)
  {
    const std::vector<std::string> key_value = SplitString(field, '=');
    if (key_value.size() != 2)
      continue;

    if (key_value[0] == key)
      return std::string{StripWhitespace(key_value[1])};
  }

  return {};
}

void NetKDRequestDevice::LogError(ErrorType error_type, s32 error_code)
{
  s32 new_code{};
  switch (error_type)
  {
  case ErrorType::Account:
    new_code = -(101200 - error_code);
    break;
  case ErrorType::Client:
    new_code = -(107300 - error_code);
    break;
  case ErrorType::KD_Download:
    new_code = -(107200 - error_code);
    break;
  case ErrorType::Server:
    new_code = -(117000 + error_code);
    break;
  case ErrorType::CheckMail:
    new_code = -(102200 - error_code);
    break;
  }

  std::lock_guard lg(m_scheduler_buffer_lock);

  m_scheduler_buffer[32 + (m_error_count % 32)] = Common::swap32(new_code);
  m_error_count++;

  m_scheduler_buffer[5] = Common::swap32(m_error_count);
  m_scheduler_buffer[2] = Common::swap32(new_code);
}

NWC24::ErrorCode NetKDRequestDevice::KDCheckMail(u32* mail_flag, u32* interval)
{
  bool success = false;
  Common::ScopeGuard state_guard([&] {
    std::lock_guard lg(m_scheduler_buffer_lock);
    if (success)
    {
      // m_scheduler_buffer[11] contains the amount of times we have checked for mail this IOS
      // session.
      m_scheduler_buffer[11] = Common::swap32(Common::swap32(m_scheduler_buffer[11]) + 1);
    }
    m_scheduler_buffer[4] = static_cast<u32>(CurrentFunction::None);
  });

  {
    std::lock_guard lg(m_scheduler_buffer_lock);
    m_scheduler_buffer[4] = Common::swap32(static_cast<u32>(CurrentFunction::Check));
  }

  u64 random_number{};
  Common::Random::Generate(&random_number, sizeof(u64));
  const std::string form_data(
      fmt::format("mlchkid={}&chlng={}", m_config.GetMlchkid(), random_number));
  const Common::HttpRequest::Response response = m_http.Post(m_config.GetCheckURL(), form_data);

  if (!response)
  {
    ERROR_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_CHECK_MAIL_NOW: Failed to request data at {}.",
                  m_config.GetCheckURL());
    return NWC24::WC24_ERR_SERVER;
  }

  const std::string response_str = {response->begin(), response->end()};
  const std::string code = GetValueFromCGIResponse(response_str, "cd");
  if (code != "100")
  {
    ERROR_LOG_FMT(
        IOS_WC24,
        "NET_KD_REQ: IOCTL_NWC24_CHECK_MAIL_NOW: Mail server returned non-success code: {}", code);
    return NWC24::WC24_ERR_SERVER;
  }

  const std::string server_hmac = GetValueFromCGIResponse(response_str, "res");
  const std::string str_mail_flag = GetValueFromCGIResponse(response_str, "mail.flag");
  const std::string str_interval = GetValueFromCGIResponse(response_str, "interval");
  DEBUG_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_CHECK_MAIL_NOW: Server HMAC: {}", server_hmac);

  // On a real Wii, a response to a challenge is expected and would be verified by KD.
  const std::string hmac_message =
      fmt::format("{}\nw{}\n{}\n{}", random_number, m_config.Id(), str_mail_flag, str_interval);
  std::array<u8, 20> hashed{};
  Common::HMAC::HMACWithSHA1(
      MAIL_CHECK_KEY,
      std::span<const u8>(reinterpret_cast<const u8*>(hmac_message.data()), hmac_message.size()),
      hashed.data());

  // On a real Wii, strncmp is used to compare both hashes. This means that it is a case-sensitive
  // comparison. KD will generate a lowercase hash as well as expect a lowercase hash from the
  // server.
  if (Common::BytesToHexString(hashed) != server_hmac)
  {
    ERROR_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_CHECK_MAIL_NOW: Server HMAC is invalid.");
    return NWC24::WC24_ERR_SERVER;
  }

  *mail_flag = std::strncmp(str_mail_flag.data(), m_send_list.GetMailFlag().data(), 22) != 0;

  {
    std::lock_guard scheduler_lg(m_scheduler_lock);
    bool did_parse = TryParse(m_http.GetHeaderValue("X-Wii-Mail-Check-Span"), interval);
    if (did_parse)
    {
      if (*interval == 0)
      {
        *interval = 1;
      }

      m_mail_span = *interval;
    }

    did_parse = TryParse(m_http.GetHeaderValue("X-Wii-Download-Span"), &m_download_span);
    if (did_parse)
    {
      if (m_download_span == 0)
      {
        m_download_span = 1;
      }
    }
  }

  success = true;
  return NWC24::WC24_OK;
}

NWC24::ErrorCode NetKDRequestDevice::KDDownload(const u16 entry_index,
                                                const std::optional<u8> subtask_id)
{
  std::vector<u8> file_data;

  // Content metadata
  const std::string content_name = m_dl_list.GetVFFContentName(entry_index, subtask_id);
  std::string url = m_dl_list.GetDownloadURL(entry_index, subtask_id);

  // Reroute to custom server if enabled.
  const std::vector<std::string> parts = SplitString(url, '/');
  if (parts.size() < 3)
  {
    // Invalid URL
    LogError(ErrorType::KD_Download, NWC24::WC24_ERR_SERVER);
    return NWC24::WC24_ERR_SERVER;
  }

  if (std::optional<std::string> patch =
          WC24PatchEngine::GetNetworkPatch(parts[2], WC24PatchEngine::IsKD{true}))
  {
    const size_t index = url.find(parts[2]);
    url.replace(index, parts[2].size(), patch.value());
  }

  INFO_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_DOWNLOAD_NOW_EX - NI - URL: {}", url);

  INFO_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_DOWNLOAD_NOW_EX - NI - Name: {}", content_name);

  const Common::HttpRequest::Response response = m_http.Get(url);

  if (!response)
  {
    const s32 last_response_code = m_http.GetLastResponseCode();
    ERROR_LOG_FMT(IOS_WC24, "Failed to request data at {}. HTTP Status Code: {}", url,
                  last_response_code);

    // On a real Wii, KD throws 107305 if it cannot connect to the host. While other issues other
    // than invalid host may arise, this code is essentially a catch-all for HTTP client failure.
    LogError(last_response_code ? ErrorType::Server : ErrorType::Client,
             last_response_code ? last_response_code : NWC24::WC24_ERR_NULL);
    return NWC24::WC24_ERR_SERVER;
  }

  if (!m_dl_list.IsRSASigned(entry_index))
  {
    // Data that is not signed with an RSA key will not have the WC24 header or 320 bytes before the
    // actual data. We just have to make sure that the response is not empty.
    if (response->empty())
    {
      ERROR_LOG_FMT(IOS_WC24, "File at {} is empty.", url);
      LogError(ErrorType::KD_Download, NWC24::WC24_ERR_BROKEN);
      return NWC24::WC24_ERR_BROKEN;
    }

    file_data = *response;
  }
  else
  {
    // Check if the filesize is smaller than the header size.
    if (response->size() < sizeof(NWC24::WC24File))
    {
      ERROR_LOG_FMT(IOS_WC24, "File at {} is too small to be a valid file.", url);
      LogError(ErrorType::KD_Download, NWC24::WC24_ERR_BROKEN);
      return NWC24::WC24_ERR_BROKEN;
    }

    // Now we read the file
    NWC24::WC24File wc24_file;
    std::memcpy(&wc24_file, response->data(), sizeof(NWC24::WC24File));

    std::vector<u8> temp_buffer(response->begin() + 320, response->end());

    if (m_dl_list.IsEncrypted(entry_index))
    {
      NWC24::WC24PubkMod pubk_mod = m_dl_list.GetWC24PubkMod(entry_index);

      file_data = std::vector<u8>(response->size() - 320);

      Common::AES::CryptOFB(pubk_mod.aes_key, wc24_file.iv, wc24_file.iv, temp_buffer.data(),
                            file_data.data(), temp_buffer.size());
    }
    else
    {
      file_data = std::move(temp_buffer);
    }
  }

  NWC24::ErrorCode reply = IOS::HLE::NWC24::OpenVFF(m_dl_list.GetVFFPath(entry_index), content_name,
                                                    m_ios.GetFS(), file_data);

  if (reply != NWC24::WC24_OK)
    LogError(ErrorType::KD_Download, reply);

  return reply;
}

IPCReply NetKDRequestDevice::HandleNWC24CheckMailNow(const IOCtlRequest& request)
{
  auto& system = GetSystem();
  auto& memory = system.GetMemory();

  u32 mail_flag{};
  u32 interval{};
  const NWC24::ErrorCode reply = KDCheckMail(&mail_flag, &interval);

  WriteReturnValue(reply, request.buffer_out);
  memory.Write_U32(mail_flag, request.buffer_out + 4);
  memory.Write_U32(interval, request.buffer_out + 8);
  return IPCReply(IPC_SUCCESS);
}

IPCReply NetKDRequestDevice::HandleNWC24DownloadNowEx(const IOCtlRequest& request)
{
  m_dl_list.ReadDlList();
  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  const u32 flags = memory.Read_U32(request.buffer_in);
  // Nintendo converts the entry ID between a u32 and u16
  // several times, presumably for alignment purposes.
  // We'll skip past buffer_in+4 and keep the entry index as a u16.
  const u16 entry_index = memory.Read_U16(request.buffer_in + 6);
  const u32 subtask_bitmask = memory.Read_U32(request.buffer_in + 8);

  INFO_LOG_FMT(IOS_WC24,
               "NET_KD_REQ: IOCTL_NWC24_DOWNLOAD_NOW_EX - NI - flags: {}, index: {}, bitmask: {}",
               flags, entry_index, subtask_bitmask);

  if (entry_index >= NWC24::NWC24Dl::MAX_ENTRIES)
  {
    ERROR_LOG_FMT(IOS_WC24, "NET_KD_REQ: Entry index out of range.");
    LogError(ErrorType::KD_Download, NWC24::WC24_ERR_INVALID_VALUE);
    WriteReturnValue(NWC24::WC24_ERR_INVALID_VALUE, request.buffer_out);
    return IPCReply(IPC_SUCCESS);
  }

  if (!m_dl_list.DoesEntryExist(entry_index))
  {
    ERROR_LOG_FMT(IOS_WC24, "NET_KD_REQ: Requested entry does not exist in download list!");
    LogError(ErrorType::KD_Download, NWC24::WC24_ERR_NOT_FOUND);
    WriteReturnValue(NWC24::WC24_ERR_NOT_FOUND, request.buffer_out);
    return IPCReply(IPC_SUCCESS);
  }

  // While in theory reply will always get initialized by KDDownload, things happen.
  // Returning NWC24::WC24_ERR_BROKEN or anything that isn't OK will prompt the channel to fix the
  // entry's data.
  NWC24::ErrorCode reply = NWC24::WC24_ERR_BROKEN;

  // Determine if we have subtasks to handle
  if (Common::ExtractBit(flags, 2))
  {
    for (u8 subtask_id = 0; subtask_id < 32; subtask_id++)
    {
      if (Common::ExtractBit(subtask_bitmask, subtask_id))
      {
        reply = KDDownload(entry_index, subtask_id);
        if (reply != NWC24::WC24_OK)
        {
          // An error has occurred, break out and return error.
          break;
        }
      }
    }
  }
  else
  {
    reply = KDDownload(entry_index, std::nullopt);
  }

  WriteReturnValue(reply, request.buffer_out);
  return IPCReply(IPC_SUCCESS);
}

std::optional<IPCReply> NetKDRequestDevice::IOCtl(const IOCtlRequest& request)
{
  enum : u32
  {
    IOCTL_NWC24_SUSPEND_SCHEDULER = 0x01,
    IOCTL_NWC24_EXEC_TRY_SUSPEND_SCHEDULER = 0x02,
    IOCTL_NWC24_EXEC_RESUME_SCHEDULER = 0x03,
    IOCTL_NWC24_KD_GET_TIME_TRIGGERS = 0x04,
    IOCTL_NWC24_SET_SCHEDULE_SPAN = 0x05,
    IOCTL_NWC24_STARTUP_SOCKET = 0x06,
    IOCTL_NWC24_CLEANUP_SOCKET = 0x07,
    IOCTL_NWC24_LOCK_SOCKET = 0x08,
    IOCTL_NWC24_UNLOCK_SOCKET = 0x09,
    IOCTL_NWC24_CHECK_MAIL_NOW = 0x0A,
    IOCTL_NWC24_SEND_MAIL_NOW = 0x0B,
    IOCTL_NWC24_RECEIVE_MAIL_NOW = 0x0C,
    IOCTL_NWC24_SAVE_MAIL_NOW = 0x0D,
    IOCTL_NWC24_DOWNLOAD_NOW_EX = 0x0E,
    IOCTL_NWC24_REQUEST_GENERATED_USER_ID = 0x0F,
    IOCTL_NWC24_REQUEST_REGISTER_USER_ID = 0x10,
    IOCTL_NWC24_GET_SCHEDULER_STAT = 0x1E,
    IOCTL_NWC24_SET_FILTER_MODE = 0x1F,
    IOCTL_NWC24_SET_DEBUG_MODE = 0x20,
    IOCTL_NWC24_KD_SET_NEXT_WAKEUP = 0x21,
    IOCTL_NWC24_SET_SCRIPT_MODE = 0x22,
    IOCTL_NWC24_REQUEST_SHUTDOWN = 0x28,
  };

  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  s32 return_value = 0;
  switch (request.request)
  {
  case IOCTL_NWC24_SUSPEND_SCHEDULER:
    // NWC24iResumeForCloseLib  from NWC24SuspendScheduler (Input: none, Output: 32 bytes)
    INFO_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_SUSPEND_SCHEDULER - NI");
    WriteReturnValue(0, request.buffer_out);  // no error
    break;

  case IOCTL_NWC24_EXEC_TRY_SUSPEND_SCHEDULER:  // NWC24iResumeForCloseLib
    INFO_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_EXEC_TRY_SUSPEND_SCHEDULER - NI");
    break;

  case IOCTL_NWC24_EXEC_RESUME_SCHEDULER:  // NWC24iResumeForCloseLib
    INFO_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_EXEC_RESUME_SCHEDULER - NI");
    WriteReturnValue(0, request.buffer_out);  // no error
    break;

  case IOCTL_NWC24_STARTUP_SOCKET:  // NWC24iStartupSocket
    WriteReturnValue(0, request.buffer_out);
    memory.Write_U32(0, request.buffer_out + 4);
    return_value = 0;
    INFO_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_STARTUP_SOCKET - NI");
    break;

  case IOCTL_NWC24_CLEANUP_SOCKET:
    INFO_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_CLEANUP_SOCKET");
    GetEmulationKernel().GetSocketManager()->Clean();
    break;

  case IOCTL_NWC24_LOCK_SOCKET:  // WiiMenu
    INFO_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_LOCK_SOCKET - NI");
    break;

  case IOCTL_NWC24_UNLOCK_SOCKET:
    INFO_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_UNLOCK_SOCKET - NI");
    break;

  case IOCTL_NWC24_REQUEST_REGISTER_USER_ID:
    INFO_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_REQUEST_REGISTER_USER_ID");
    WriteReturnValue(0, request.buffer_out);
    memory.Write_U32(0, request.buffer_out + 4);
    break;

  case IOCTL_NWC24_REQUEST_GENERATED_USER_ID:  // (Input: none, Output: 32 bytes)
    INFO_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_REQUEST_GENERATED_USER_ID");
    if (m_config.IsCreated())
    {
      const std::string settings_file_path =
          Common::GetTitleDataPath(Titles::SYSTEM_MENU) + "/" WII_SETTING;
      std::string area, model;

      const auto fs = m_ios.GetFS();
      if (const auto file = fs->OpenFile(PID_KD, PID_KD, settings_file_path, FS::Mode::Read))
      {
        Common::SettingsHandler::Buffer data;
        if (file->Read(data.data(), data.size()))
        {
          const Common::SettingsHandler gen{std::move(data)};
          area = gen.GetValue("AREA");
          model = gen.GetValue("MODEL");
        }
      }

      if (!area.empty() && !model.empty())
      {
        const u8 area_code = GetAreaCode(area);
        const u8 id_ctr = u8(m_config.IdGen());
        const HardwareModel hardware_model = GetHardwareModel(model);

        const u32 hollywood_id = m_ios.GetIOSC().GetDeviceId();
        u64 user_id = 0;

        const s32 ret = NWC24MakeUserID(&user_id, hollywood_id, id_ctr, hardware_model, area_code);
        m_config.SetId(user_id);
        m_config.IncrementIdGen();
        m_config.SetCreationStage(NWC24::NWC24CreationStage::Generated);
        m_config.SetChecksum(m_config.CalculateNwc24ConfigChecksum());
        m_config.WriteConfig();
        m_config.WriteCBK();

        WriteReturnValue(ret, request.buffer_out);
      }
      else
      {
        LogError(ErrorType::Account, NWC24::WC24_ERR_INVALID_VALUE);
        WriteReturnValue(NWC24::WC24_ERR_FATAL, request.buffer_out);
      }
    }
    else if (m_config.IsGenerated())
    {
      WriteReturnValue(NWC24::WC24_ERR_ID_GENERATED, request.buffer_out);
    }
    else if (m_config.IsRegistered())
    {
      WriteReturnValue(NWC24::WC24_ERR_ID_REGISTERED, request.buffer_out);
    }
    memory.Write_U64(m_config.Id(), request.buffer_out + 4);
    memory.Write_U32(u32(m_config.CreationStage()), request.buffer_out + 0xC);
    break;

  case IOCTL_NWC24_GET_SCHEDULER_STAT:
  {
    if (request.buffer_out == 0 || request.buffer_out % 4 != 0 || request.buffer_out_size < 16)
    {
      return_value = IPC_EINVAL;
      ERROR_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_GET_SCHEDULER_STAT = IPC_EINVAL");
      break;
    }

    INFO_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_GET_SCHEDULER_STAT - buffer out size: {}",
                 request.buffer_out_size);

    // On a real Wii, GetSchedulerStat copies memory containing a list of error codes recorded by
    // KD among other things.
    std::lock_guard lg(m_scheduler_buffer_lock);
    const u32 out_size = std::min(request.buffer_out_size, 256U);
    memory.CopyToEmu(request.buffer_out, m_scheduler_buffer.data(), out_size);
    break;
  }

  case IOCTL_NWC24_SAVE_MAIL_NOW:
    INFO_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_SAVE_MAIL_NOW - NI");
    break;

  case IOCTL_NWC24_CHECK_MAIL_NOW:
    return LaunchAsyncTask(&NetKDRequestDevice::HandleNWC24CheckMailNow, request);

  case IOCTL_NWC24_DOWNLOAD_NOW_EX:
    return LaunchAsyncTask(&NetKDRequestDevice::HandleNWC24DownloadNowEx, request);

  case IOCTL_NWC24_REQUEST_SHUTDOWN:
  {
    if (request.buffer_in == 0 || request.buffer_in % 4 != 0 || request.buffer_in_size < 8 ||
        request.buffer_out == 0 || request.buffer_out % 4 != 0 || request.buffer_out_size < 4)
    {
      return_value = IPC_EINVAL;
      ERROR_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_REQUEST_SHUTDOWN = IPC_EINVAL");
      break;
    }

    INFO_LOG_FMT(IOS_WC24, "NET_KD_REQ: IOCTL_NWC24_REQUEST_SHUTDOWN");
    [[maybe_unused]] const u32 event = memory.Read_U32(request.buffer_in);
    // TODO: Advertise shutdown event
    // TODO: Shutdown USB keyboard LEDs if event == 3
    // TODO: IOCTLV_NCD_SETCONFIG
    // TODO: DHCP related features
    // SOGetInterfaceOpt(0xfffe,0x4003);  // IP settings
    // SOGetInterfaceOpt(0xfffe,0xc001);  // DHCP lease time remaining?
    // SOGetInterfaceOpt(0xfffe,0x1003);  // Error
    // Call /dev/net/ip/top 0x1b (SOCleanup), it closes all sockets
    GetEmulationKernel().GetSocketManager()->Clean();
    return_value = IPC_SUCCESS;
    break;
  }

  default:
    request.Log(GetDeviceName(), Common::Log::LogType::IOS_WC24);
  }

  return IPCReply(return_value);
}
}  // namespace IOS::HLE
