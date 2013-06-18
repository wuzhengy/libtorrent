/*

Copyright (c) 2012, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "save_resume.hpp"

#include <boost/bind.hpp>
#include <boost/tuple/tuple.hpp> // for boost::tie

#include "libtorrent/add_torrent_params.hpp"
#include "libtorrent/session.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/entry.hpp"
#include "libtorrent/bencode.hpp"
#include "libtorrent/lazy_entry.hpp"
#include "libtorrent/file.hpp"
#include "libtorrent/alert_handler.hpp"
#include "libtorrent/alert_types.hpp"

namespace libtorrent
{

// defined in save_settings.cpp
int save_file(std::string const& filename, std::vector<char>& v, error_code& ec);

save_resume::save_resume(session& s, std::string const& resume_dir, alert_handler* alerts)
	: m_ses(s)
	, m_alerts(alerts)
	, m_resume_dir(resume_dir)
	, m_cursor(m_torrents.begin())
	, m_last_save(time_now())
	, m_interval(minutes(5))
	, m_num_in_flight(0)
{
	m_alerts->subscribe(this, 0, add_torrent_alert::alert_type
		, torrent_removed_alert::alert_type
		, stats_alert::alert_type // just to get woken up regularly
		, save_resume_data_alert::alert_type
		, metadata_received_alert::alert_type, 0);
}

save_resume::~save_resume()
{
	m_alerts->unsubscribe(this);
}

void save_resume::handle_alert(alert const* a)
{
	add_torrent_alert const* ta = alert_cast<add_torrent_alert>(a);
	torrent_removed_alert const* td = alert_cast<torrent_removed_alert>(a);
	save_resume_data_alert const* sr = alert_cast<save_resume_data_alert>(a);
	save_resume_data_failed_alert const* sf = alert_cast<save_resume_data_failed_alert>(a);
	metadata_received_alert const* mr = alert_cast<metadata_received_alert>(a);
	if (ta)
	{
		torrent_status st = ta->handle.status();
		printf("added torrent: %s\n", st.name.c_str());
		m_torrents.insert(ta->handle);
		if (st.has_metadata)
		{
			ta->handle.save_resume_data(torrent_handle::save_info_dict);
			++m_num_in_flight;
		}

		if (m_cursor == m_torrents.end())
			m_cursor = m_torrents.begin();
	}
	else if (mr)
	{
		mr->handle.save_resume_data(torrent_handle::save_info_dict);
		++m_num_in_flight;
	}
	else if (td)
	{
		bool wrapped = false;
		boost::unordered_set<torrent_handle>::iterator i = m_torrents.find(td->handle);
		TORRENT_ASSERT(i != m_torrents.end());
		if (m_cursor == i)
		{
			++m_cursor;
			if (m_cursor == m_torrents.end())
				wrapped = true;
		}
		// we need to delete the resume file from the resume directory
		// as well, to prevent it from being reloaded on next startup
		error_code ec;
		std::string resume_file = combine_path(m_resume_dir, to_hex(td->info_hash.to_string()) + ".resume");
		printf("removing: %s (%s)\n", resume_file.c_str(), ec.message().c_str());
		remove(resume_file, ec);
		m_torrents.erase(i);
		if (wrapped)
			m_cursor = m_torrents.begin();
	}
	else if (sr)
	{
		TORRENT_ASSERT(m_num_in_flight > 0);
		--m_num_in_flight;
		error_code ec;
		std::vector<char> buf;
		bencode(std::back_inserter(buf), *sr->resume_data);
		create_directory(m_resume_dir, ec);
		ec.clear();
		save_file(combine_path(m_resume_dir
			, to_hex((*sr->resume_data)["info-hash"].string()) + ".resume"), buf, ec);
	}
	else if (sf)
	{
		TORRENT_ASSERT(m_num_in_flight > 0);
		--m_num_in_flight;
	}
	
	// is it time to save resume data for another torrent?
	if (m_torrents.empty()) return;

	// calculate how many torrents we should save this tick. It depends on
	// how long since we last tried to save one. Every m_interval seconds,
	// we should have saved all torrents
	int num_torrents = m_torrents.size();
	int num_to_save = num_torrents * total_seconds(time_now() - m_last_save)
		/ total_seconds(m_interval);
	// never save more than all torrents
	num_to_save = (std::min)(num_to_save, num_torrents);

	while (num_to_save > 0)
	{
		if (m_cursor == m_torrents.end())
			m_cursor = m_torrents.begin();

		if (m_cursor->need_save_resume_data())
		{
			m_cursor->save_resume_data(torrent_handle::save_info_dict);
			printf("saving resume data for: %s\n", m_cursor->status().name.c_str());
			++m_num_in_flight;
		}
		m_last_save = time_now();
		--num_to_save;
		++m_cursor;
	}
}

void save_resume::save_all()
{
	for (boost::unordered_set<torrent_handle>::iterator i = m_torrents.begin()
		, end(m_torrents.end()); i != end; ++i)
	{
		if (!i->need_save_resume_data()) continue;
		i->save_resume_data(torrent_handle::save_info_dict);
		++m_num_in_flight;
	}
}

void save_resume::load(error_code& ec, add_torrent_params model)
{
	for (directory dir(m_resume_dir, ec); !ec && !dir.done(); dir.next(ec))
	{
		if (extension(dir.file()) != ".resume") continue;

		error_code tec;
		std::string file_path = combine_path(m_resume_dir, dir.file());
		std::vector<char> resume;
		printf("loading %s\n", file_path.c_str());
		if (load_file(file_path, resume, tec) < 0 || tec)
			continue;

		add_torrent_params p = model;

		p.resume_data = &resume;
		printf("async add\n");
		m_ses.async_add_torrent(p);
	}
}

}

