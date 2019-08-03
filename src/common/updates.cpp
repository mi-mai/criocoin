// Copyright (c) 2019, The NERVA Project
// Copyright (c) 2017-2019, The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <boost/algorithm/string.hpp>
#include "misc_log_ex.h"
#include "util.h"
#include "dns_utils.h"
#include "updates.h"

#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "updates"

namespace tools
{
  bool check_updates(const std::string &software, std::string &version, std::string &codename, std::string &notice)
  {
    std::vector<std::string> records;
    bool found = false;

    static const std::vector<std::string> dns_urls = {
      "update.getnerva.org",
    };

    if (!tools::dns_utils::load_txt_records_from_dns(records, dns_urls))
      return false;

    for (const auto& record : records)
    {
      std::vector<std::string> fields;
      boost::split(fields, record, boost::is_any_of(":"));
      if (fields.size() != 4)
      {
        MWARNING("Updates record does not have 4 fields: " << record);
        continue;
      }

      if (software != fields[0])
        continue;

      // use highest version
      if (found)
      {
        int cmp = vercmp(version.c_str(), fields[1].c_str());
        if (cmp > 0)
          continue;
      }

      version = fields[1];
      codename = fields[2];
      notice = fields[3];

      LOG_PRINT_L1("Found new version " << version << ":" << codename);
      found = true;
    }
    return found;
  }

  std::string get_update_url(const std::string &software, const std::string &buildtag, const std::string &version)
  {
    return "https://getnerva.org/content/binaries/" + software + "-v" + version + "_" + buildtag + ".zip";
  }
}
