/*
  Neutrino-GUI  -   DBoxII-Project

  Movieplayer (c) 2003, 2004 by gagga
  Based on code by Dirch, obi and the Metzler Bros. Thanks.
  (C) 2011 Stefan Seyfried

  Copyright (C) 2011 CoolStream International Ltd

  License: GPL

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define __STDC_CONSTANT_MACROS
#include <global.h>
#include <neutrino.h>

#include <gui/audiomute.h>
#include <gui/audio_select.h>
#include <gui/epgview.h>
#include <gui/eventlist.h>
#include <gui/movieplayer.h>
#include <gui/infoviewer.h>
#include <gui/timeosd.h>
#include <gui/widget/helpbox.h>
#include <gui/infoclock.h>
#include <gui/plugins.h>
#include <gui/videosettings.h>
#include <gui/widget/messagebox.h>
#include <driver/screenshot.h>
#include <driver/volume.h>
#include <driver/display.h>
#include <driver/abstime.h>
#include <system/helpers.h>

#include <unistd.h>
#include <stdlib.h>
#include <sys/timeb.h>
#include <sys/mount.h>

#include <video.h>
#include <libtuxtxt/teletext.h>
#include <zapit/zapit.h>
#include <system/set_threadname.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <iconv.h>
#if HAVE_SPARK_HARDWARE || HAVE_DUCKBOX_HARDWARE
#include <audio.h>
#endif
#ifdef ENABLE_GRAPHLCD
#include <driver/nglcd.h>
bool glcd_play = false;
#endif

//extern CPlugins *g_PluginList;
#if !HAVE_SPARK_HARDWARE && !HAVE_DUCKBOX_HARDWARE
#define LCD_MODE CVFD::MODE_MOVIE
#else
#define LCD_MODE CVFD::MODE_MENU_UTF8
#endif

extern cVideo * videoDecoder;
extern CRemoteControl *g_RemoteControl;	/* neutrino.cpp */
extern CInfoClock *InfoClock;
#if HAVE_SPARK_HARDWARE || HAVE_DUCKBOX_HARDWARE
extern cAudio * audioDecoder;
#endif

#define TIMESHIFT_SECONDS 3
#define ISO_MOUNT_POINT "/media/iso"

CMoviePlayerGui* CMoviePlayerGui::instance_mp = NULL;

CMoviePlayerGui& CMoviePlayerGui::getInstance()
{
	if ( !instance_mp )
	{
		instance_mp = new CMoviePlayerGui();
		printf("[neutrino CMoviePlayerGui] Instance created...\n");
	}
	return *instance_mp;
}

CMoviePlayerGui::CMoviePlayerGui()
{
	Init();
}

CMoviePlayerGui::~CMoviePlayerGui()
{
	playback->Close();
	delete moviebrowser;
	delete filebrowser;
	delete bookmarkmanager;
	delete playback;
	delete webtv;
	filelist.clear();
	instance_mp = NULL;
}

void getPlayerPts(int64_t *pts)
{
	//playback->GetPosition(position, duration)
	//*pts = CMoviePlayerGui::getInstance().Pts();
	cPlayback *playback = CMoviePlayerGui::getInstance().getPlayback();
	if (playback) {
		int position, duration;
		playback->GetPosition(position, duration);
		*pts = position * 90;
	}
}

uint64_t CMoviePlayerGui::GetPts(void)
{
#ifndef INVALID_PTS_VALUE
#define INVALID_PTS_VALUE 0x200000000ull
#endif
	uint64_t pts = INVALID_PTS_VALUE;
#if !HAVE_COOL_HARDWARE
	playback->GetPts(pts);
#endif
	return pts;
}
void CMoviePlayerGui::Init(void)
{
	playing = false;

	frameBuffer = CFrameBuffer::getInstance();

	playback = new cPlayback(0);
	moviebrowser = new CMovieBrowser();
	bookmarkmanager = new CBookmarkManager();
	webtv = new CWebTV();

	tsfilefilter.addFilter("ts");
#if HAVE_TRIPLEDRAGON
	tsfilefilter.addFilter("vdr");
#else
	tsfilefilter.addFilter("avi");
	tsfilefilter.addFilter("mkv");
	tsfilefilter.addFilter("wav");
	tsfilefilter.addFilter("asf");
	tsfilefilter.addFilter("aiff");
#endif
	tsfilefilter.addFilter("mpg");
	tsfilefilter.addFilter("mpeg");
	tsfilefilter.addFilter("m2p");
	tsfilefilter.addFilter("mpv");
	tsfilefilter.addFilter("vob");
	tsfilefilter.addFilter("m2ts");
	tsfilefilter.addFilter("mp4");
	tsfilefilter.addFilter("mov");
	tsfilefilter.addFilter("m3u");
	tsfilefilter.addFilter("pls");
	tsfilefilter.addFilter("iso");
#if HAVE_SPARK_HARDWARE || HAVE_DUCKBOX_HARDWARE
	tsfilefilter.addFilter("trp");
	tsfilefilter.addFilter("vdr");
	tsfilefilter.addFilter("mp3");
	tsfilefilter.addFilter("flv");
	tsfilefilter.addFilter("wmv");
#endif

	if (g_settings.network_nfs_moviedir.empty())
		Path_local = "/";
	else
		Path_local = g_settings.network_nfs_moviedir;

	if (g_settings.filebrowser_denydirectoryleave)
		filebrowser = new CFileBrowser(Path_local.c_str());
	else
		filebrowser = new CFileBrowser();

	filebrowser->Filter = &tsfilefilter;
	filebrowser->Hide_records = true;

	speed = 1;
	timeshift = 0;
	numpida = 0;
	showStartingHint = false;

	filelist_it = filelist.end();
	min_x = 0;
	max_x = 0;
	min_y = 0;
	max_y = 0;
	ext_subs = false;
	iso_file = false;
}

void CMoviePlayerGui::cutNeutrino()
{
	if (playing)
		return;

	playing = true;
	/* set g_InfoViewer update timer to 1 sec, should be reset to default from restoreNeutrino->set neutrino mode  */
	g_InfoViewer->setUpdateTimer(1000 * 1000);

	if (isUPNP)
		return;

	g_Zapit->lockPlayBack();
	g_Sectionsd->setPauseScanning(true);

#ifdef HAVE_AZBOX_HARDWARE
	/* we need sectionsd to get idle and zapit to release the demuxes
	 * and decoders so that the external player can do its work
	 * TODO: what about timeshift? */
	g_Sectionsd->setServiceChanged(0, false);
	g_Zapit->setStandby(true);
#endif
	CNeutrinoApp::getInstance()->handleMsg(NeutrinoMessages::CHANGEMODE, NeutrinoMessages::mode_ts);
	m_LastMode = (CNeutrinoApp::getInstance()->getLastMode() | NeutrinoMessages::norezap);
}

void CMoviePlayerGui::restoreNeutrino()
{
	if (!playing)
		return;

	playing = false;
#ifdef HAVE_AZBOX_HARDWARE
	g_Zapit->setStandby(false);
	CZapit::getInstance()->SetVolume(CZapit::getInstance()->GetVolume());
#endif

	if (isUPNP)
		return;

	g_Zapit->unlockPlayBack();
	g_Sectionsd->setPauseScanning(false);

	if (m_LastMode == NeutrinoMessages::mode_tv)
		g_RCInput->postMsg(NeutrinoMessages::EVT_PROGRAMLOCKSTATUS, 0x200, false);

	if (m_LastMode != NeutrinoMessages::mode_unknown)
	CNeutrinoApp::getInstance()->handleMsg(NeutrinoMessages::CHANGEMODE, m_LastMode);
	if (m_LastMode == NeutrinoMessages::mode_tv) {
		CZapitChannel *channel = CZapit::getInstance()->GetCurrentChannel();
		if (channel && channel->scrambled)
			 CZapit::getInstance()->Rezap();
	}
}

#if HAVE_SPARK_HARDWARE || HAVE_DUCKBOX_HARDWARE
static bool running = false;
#endif
int CMoviePlayerGui::exec(CMenuTarget * parent, const std::string & actionKey)
{
	printf("[movieplayer] actionKey=%s\n", actionKey.c_str());
#if HAVE_SPARK_HARDWARE || HAVE_DUCKBOX_HARDWARE
	if (running)
		return menu_return::RETURN_EXIT_ALL;
	running = true;
#endif

	if (parent)
		parent->hide();

	if (!access(MOVIEPLAYER_START_SCRIPT, X_OK)) {
	puts("[movieplayer.cpp] executing " MOVIEPLAYER_START_SCRIPT ".");
	if (my_system(MOVIEPLAYER_START_SCRIPT) != 0)
		perror(MOVIEPLAYER_START_SCRIPT " failed");
	}

	Cleanup();

	isMovieBrowser = false;
	isBookmark = false;
	timeshift = 0;
	isHTTP = false;
	isUPNP = false;
	isWebTV = false;

	if (actionKey == "tsmoviebrowser") {
		isMovieBrowser = true;
		moviebrowser->setMode(MB_SHOW_RECORDS);
	}
	else if (actionKey == "ytplayback") {
		CAudioMute::getInstance()->enableMuteIcon(false);
		InfoClock->enableInfoClock(false);
		isMovieBrowser = true;
		moviebrowser->setMode(MB_SHOW_YT);
	}
	else if (actionKey == "fileplayback") {
	}
	else if (actionKey == "timeshift") {
		timeshift = 1;
	}
	else if (actionKey == "ptimeshift") {
		timeshift = 2;
	}
	else if (actionKey == "rtimeshift") {
		timeshift = 3;
	}
#if 0 // TODO ?
	else if (actionKey == "bookmarkplayback") {
		isBookmark = true;
	}
#endif
	else if (actionKey == "webtv") {
		isWebTV = true;
	}
	else if (actionKey == "netstream") {
		isHTTP = true;
		full_name = g_settings.streaming_server_url;
		file_name = (isWebTV ? g_settings.streaming_server_name : g_settings.streaming_server_url);
		p_movie_info = NULL;
		is_file_player = 1;
		PlayFile();
	}
	else if (actionKey == "upnp") {
		isUPNP = true;
		is_file_player = 1;
		PlayFile();
	}
	else if (actionKey == "http") {
		isHTTP = true;
		is_file_player = 1;
		PlayFile();
	}
	else {
#if HAVE_SPARK_HARDWARE || HAVE_DUCKBOX_HARDWARE
		running = false;
#endif
		return menu_return::RETURN_REPAINT;
	}

	repeat_mode = REPEAT_OFF;
#if HAVE_SPARK_HARDWARE || HAVE_DUCKBOX_HARDWARE
	std::string oldservicename = CVFD::getInstance()->getServicename();
#endif
	while(!isHTTP && !isUPNP && SelectFile()) {
#if HAVE_SPARK_HARDWARE || HAVE_DUCKBOX_HARDWARE
		CVFD::getInstance()->setMode(CVFD::MODE_TVRADIO);
		if (isWebTV)
			CVFD::getInstance()->showServicename(file_name.c_str());
		else
			CVFD::getInstance()->showServicename(full_name.c_str());
#endif
		if(timeshift) {
		PlayFile();
			break;
	}
		do
			PlayFile();
		while (repeat_mode);
	}
#if HAVE_SPARK_HARDWARE || HAVE_DUCKBOX_HARDWARE
	CVFD::getInstance()->showServicename(oldservicename.c_str());
#endif

	bookmarkmanager->flush();

	if (!access(MOVIEPLAYER_END_SCRIPT, X_OK)) {
	puts("[movieplayer.cpp] executing " MOVIEPLAYER_END_SCRIPT ".");
	if (my_system(MOVIEPLAYER_END_SCRIPT) != 0)
		perror(MOVIEPLAYER_END_SCRIPT " failed");
	}

	CVFD::getInstance()->setMode(CVFD::MODE_TVRADIO);

#if HAVE_SPARK_HARDWARE || HAVE_DUCKBOX_HARDWARE
	running = false;
#endif
	if (moviebrowser->getMode() == MB_SHOW_YT) {
		CAudioMute::getInstance()->enableMuteIcon(true);
		InfoClock->enableInfoClock(true);
	}

	if (timeshift){
		timeshift = 0;
		return menu_return::RETURN_EXIT_ALL;
	}
	return menu_ret; //menu_return::RETURN_REPAINT;
}

void CMoviePlayerGui::updateLcd()
{
	char tmp[20];
	std::string lcd;
	std::string name;

	if (isMovieBrowser && strlen(p_movie_info->epgTitle.c_str()) && strncmp(p_movie_info->epgTitle.c_str(), "not", 3))
		name = p_movie_info->epgTitle;
	else
		name = file_name;

	switch (playstate) {
		case CMoviePlayerGui::PAUSE:
#if !defined(BOXMODEL_UFS910) && !defined(BOXMODEL_UFS912) && !defined(BOXMODEL_UFS913) && !defined(BOXMODEL_UFS922)
			lcd = "|| ";
#else
			lcd = "";
#endif
			if (speed < 0) {
				sprintf(tmp, "%dx<| ", abs(speed));
				lcd = tmp;
			} else if (speed > 0) {
				sprintf(tmp, "%dx|> ", abs(speed));
				lcd = tmp;
#if !defined(BOXMODEL_UFS910) && !defined(BOXMODEL_UFS912) && !defined(BOXMODEL_UFS913) && !defined(BOXMODEL_UFS922)
			} else
				lcd = "|| ";
#else
			} else
				lcd = "";
#endif
			break;
		case CMoviePlayerGui::REW:
			sprintf(tmp, "%dx<< ", abs(speed));
			lcd = tmp;
			break;
		case CMoviePlayerGui::FF:
			sprintf(tmp, "%dx>> ", abs(speed));
			lcd = tmp;
			break;
		case CMoviePlayerGui::PLAY:
#if !defined(BOXMODEL_UFS910) && !defined(BOXMODEL_UFS912) && !defined(BOXMODEL_UFS913) && !defined(BOXMODEL_UFS922) && !defined(BOXMODEL_CUBEREVO_MINI2)
			lcd = "> ";
#endif
			break;
		default:
			break;
	}
	lcd += name;
	CVFD::getInstance()->setMode(LCD_MODE);
	CVFD::getInstance()->showMenuText(0, lcd.c_str(), -1, true);
}

void CMoviePlayerGui::fillPids()
{
	if(p_movie_info == NULL)
		return;

	numpida = 0; currentapid = 0;
	if(!p_movie_info->audioPids.empty()) {
		currentapid = p_movie_info->audioPids[0].epgAudioPid;
		currentac3 = p_movie_info->audioPids[0].atype;
	}
	for (unsigned int i = 0; i < p_movie_info->audioPids.size(); i++) {
		unsigned int j;
		for (j = 0; j < numpida && p_movie_info->audioPids[i].epgAudioPid != apids[j]; j++);
		if (j == numpida) {
			apids[i] = p_movie_info->audioPids[i].epgAudioPid;
			ac3flags[i] = p_movie_info->audioPids[i].atype;
			numpida++;
			if (p_movie_info->audioPids[i].selected) {
				currentapid = p_movie_info->audioPids[i].epgAudioPid;
				currentac3 = p_movie_info->audioPids[i].atype;
			}
#if HAVE_SPARK_HARDWARE || HAVE_DUCKBOX_HARDWARE
			if (numpida == REC_MAX_APIDS)
				break;
#endif
		}
	}
	vpid = p_movie_info->epgVideoPid;
	vtype = p_movie_info->VideoType;
}

void CMoviePlayerGui::Cleanup()
{
	/*clear audiopids */
	for (int i = 0; i < numpida; i++) {
		apids[i] = 0;
		ac3flags[i] = 0;
		language[i].clear();
	}
	numpida = 0; currentapid = 0;
	currentspid = -1;
	numsubs = 0;
	vpid = 0;
	vtype = 0;

	startposition = 0;
	is_file_player = false;
	p_movie_info = NULL;
	autoshot_done = false;
}

void CMoviePlayerGui::makeFilename()
{
	if(file_name.empty()) {
		std::string::size_type pos = full_name.find_last_of('/');
		if(pos != std::string::npos) {
			file_name = full_name.substr(pos+1);
			std::replace(file_name.begin(), file_name.end(), '_', ' ');
		} else
			file_name = full_name;
		
		if(file_name.substr(0,14)=="videoplayback?"){//youtube name
			if(!p_movie_info->epgTitle.empty())
				file_name = p_movie_info->epgTitle;
			else
				file_name = "";
		}
		printf("CMoviePlayerGui::makeFilename: full_name [%s] file_name [%s]\n", full_name.c_str(), file_name.c_str());
	}
}
bool CMoviePlayerGui::SelectFile()
{
	bool ret = false;
	menu_ret = menu_return::RETURN_REPAINT;

	Cleanup();
	file_name = "";
	full_name = "";

	printf("CMoviePlayerGui::SelectFile: isBookmark %d timeshift %d isMovieBrowser %d\n", isBookmark, timeshift, isMovieBrowser);
	wakeup_hdd(g_settings.network_nfs_recordingdir.c_str());

	if (timeshift) {
		t_channel_id live_channel_id = CZapit::getInstance()->GetCurrentChannelID();
		p_movie_info = CRecordManager::getInstance()->GetMovieInfo(live_channel_id);
		full_name = CRecordManager::getInstance()->GetFileName(live_channel_id) + ".ts";
		fillPids();
		ret = true;
	}
#if 0 // TODO
	else if(isBookmark) {
		const CBookmark * theBookmark = bookmarkmanager->getBookmark(NULL);
		if (theBookmark == NULL) {
			bookmarkmanager->flush();
			return false;
		}
		full_name = theBookmark->getUrl();
		sscanf(theBookmark->getTime(), "%lld", &startposition);
		startposition *= 1000;
		ret = true;
	}
#endif
	else if (isMovieBrowser) {
		if (moviebrowser->exec(Path_local.c_str())) {
			// get the current path and file name
			Path_local = moviebrowser->getCurrentDir();
			CFile *file;
			if ((file = moviebrowser->getSelectedFile()) != NULL) {
				// get the movie info handle (to be used for e.g. bookmark handling)
				p_movie_info = moviebrowser->getCurrentMovieInfo();
				if (moviebrowser->getMode() == MB_SHOW_RECORDS) {
					full_name = file->Name;
				}
				else if (moviebrowser->getMode() == MB_SHOW_YT) {
					full_name = file->Url;
					is_file_player = true;
				}
				fillPids();

				// get the start position for the movie
				startposition = 1000 * moviebrowser->getCurrentStartPos();
				printf("CMoviePlayerGui::SelectFile: file %s start %d apid %X atype %d vpid %x vtype %d\n", full_name.c_str(), startposition, currentapid, currentac3, vpid, vtype);

				ret = true;
			}
		} else
			menu_ret = moviebrowser->getMenuRet();
	} else if (isWebTV) {
		if (webtv->getFile(file_name, full_name)) {
			is_file_player = true;
			fillPids();
			ret = true;
		}
	} else if (filelist.size() > 0 && repeat_mode == REPEAT_TRACK) {
		--filelist_it;
		is_file_player = true;
		full_name = (*filelist_it).Name;
		++filelist_it;
		ret = true;
	} else if (filelist.size() > 0 && filelist_it == filelist.end() && repeat_mode == REPEAT_ALL) {
		filelist_it = filelist.begin();
		is_file_player = true;
		full_name = (*filelist_it).Name;
		++filelist_it;
		ret = true;
	} else if (filelist.size() > 0 && filelist_it != filelist.end()) {
		is_file_player = true;
		full_name = (*filelist_it).Name;
		++filelist_it;
		ret = true;
	}
	else { // filebrowser
		CAudioMute::getInstance()->enableMuteIcon(false);
		filebrowser->Multi_Select = g_settings.filebrowser_multi_select;
		InfoClock->enableInfoClock(false);
		if (filebrowser->exec(Path_local.c_str()) == true) {
			Path_local = filebrowser->getCurrentDir();
			CFile *file = filebrowser->getSelectedFile();
			filelist = filebrowser->getSelectedFiles();
			filelist_it = filelist.end();
			if (filelist.size() > 1) {
				filelist_it = filelist.begin();
				is_file_player = true;
				full_name = (*filelist_it).Name;
				++filelist_it;
				ret = true;
			} else if (file) {
				is_file_player = true;
				full_name = file->Name;
				ret = true;
				if(file->getType() == CFile::FILE_PLAYLIST) {
					std::ifstream infile;
					char cLine[1024];
					char name[1024] = { 0 };
					infile.open(file->Name.c_str(), std::ifstream::in);
					while (infile.good())
					{
						infile.getline(cLine, sizeof(cLine));
						if (cLine[strlen(cLine)-1]=='\r')
							cLine[strlen(cLine)-1]=0;

						int dur;
						sscanf(cLine, "#EXTINF:%d,%[^\n]\n", &dur, name);
						if (strlen(cLine) > 0 && cLine[0]!='#')
						{
							char *url = NULL;
							if ( (url = strstr(cLine, "http://")) || (url = strstr(cLine, "rtmp://")) || (url = strstr(cLine, "rtsp://")) ){
								if (url != NULL) {
									printf("name %s [%d] url: %s\n", name, dur, url);
									full_name = url;
									if(strlen(name))
										file_name = name;
								}
							}
						}
					}
				}
				else if(file->getType() == CFile::FILE_ISO)
					ret = mountIso(file);
			}
		} else
			menu_ret = filebrowser->getMenuRet();
		CAudioMute::getInstance()->enableMuteIcon(true);
		InfoClock->enableInfoClock(true);
	}
	if(ret && file_name.empty()) {
		std::string::size_type pos = full_name.find_last_of('/');
		if(pos != std::string::npos) {
			file_name = full_name.substr(pos+1);
			std::replace(file_name.begin(), file_name.end(), '_', ' ');
		} else
			file_name = full_name;
		
		if(file_name.substr(0,14)=="videoplayback?"){//youtube name
			if(!p_movie_info->epgTitle.empty())
				file_name = p_movie_info->epgTitle;
			else
				file_name = "";
		}
		printf("CMoviePlayerGui::SelectFile: full_name [%s] file_name [%s]\n", full_name.c_str(), file_name.c_str());
	}
	//store last multiformat play dir
	g_settings.network_nfs_moviedir = Path_local;

	return ret;
}

void *CMoviePlayerGui::ShowStartHint(void *arg)
{
	set_threadname(__func__);
	CMoviePlayerGui *caller = (CMoviePlayerGui *)arg;
	CHintBox *hintbox = NULL;
	if(!caller->file_name.empty() && (caller->isWebTV )){
		hintbox = new CHintBox(LOCALE_MOVIEPLAYER_STARTING, caller->file_name.c_str(), 450, NEUTRINO_ICON_MOVIEPLAYER);
		hintbox->paint();
	}
	while (caller->showStartingHint) {
		neutrino_msg_t msg;
		neutrino_msg_data_t data;
		g_RCInput->getMsg(&msg, &data, 1);
		if (msg == CRCInput::RC_home || msg == CRCInput::RC_stop) {
			if(caller->playback)
				caller->playback->RequestAbort();
		} else if (msg != CRCInput::RC_timeout && msg > CRCInput::RC_MaxRC) {
			CNeutrinoApp::getInstance()->handleMsg(msg, data);
		}
	}
	if(hintbox != NULL){
		hintbox->hide();
		delete hintbox;
	}
	return NULL;
}

void CMoviePlayerGui::PlayFile(void)
{
	neutrino_msg_t msg;
	neutrino_msg_data_t data;
	menu_ret = menu_return::RETURN_REPAINT;

	bool first_start = true;
	bool time_forced = false;
	bool update_lcd = true;
	int eof = 0;

	//CTimeOSD FileTime;
	position = 0, duration = 0;
	speed = 1;

	playstate = CMoviePlayerGui::STOPPED;
	printf("Startplay at %d seconds\n", startposition/1000);
	handleMovieBrowser(CRCInput::RC_nokey, position);

	cutNeutrino();
	clearSubtitle();

	playback->Open(is_file_player ? PLAYMODE_FILE : PLAYMODE_TS);

	printf("IS FILE PLAYER: %s\n", is_file_player ?  "true": "false" );

	MI_MOVIE_INFO mi;

	if (p_movie_info) {
		if (timeshift != TSHIFT_MODE_OFF) {
		// p_movie_info may be invalidated by CRecordManager while we're still using it. Create and use a copy.
			mi = *p_movie_info;
			p_movie_info = &mi;
		}

		duration = p_movie_info->length * 60 * 1000;
		int percent = CZapit::getInstance()->GetPidVolume(p_movie_info->epgId, currentapid, currentac3 == 1);
		CZapit::getInstance()->SetVolumePercent(percent);
	}

	file_prozent = 0;
#if HAVE_SPARK_HARDWARE || HAVE_DUCKBOX_HARDWARE
	old3dmode = frameBuffer->get3DMode();
#ifdef ENABLE_GRAPHLCD
	if (p_movie_info)
		nGLCD::lockChannel(p_movie_info->epgChannel, p_movie_info->epgTitle);
	else {
		glcd_play = true;
		if (isWebTV)
			nGLCD::lockChannel(g_Locale->getText(LOCALE_WEBTV_HEAD), file_name.c_str(), file_prozent);
		else
			nGLCD::lockChannel(g_Locale->getText(LOCALE_MOVIEPLAYER_HEAD), file_name.c_str(), file_prozent);
	}
#endif
#endif
	pthread_t thrStartHint = 0;
	if (is_file_player || isWebTV) {
		showStartingHint = true;
		pthread_create(&thrStartHint, NULL, CMoviePlayerGui::ShowStartHint, this);
	}
	bool res = playback->Start((char *) full_name.c_str(), vpid, vtype, currentapid, currentac3, duration);
	if (thrStartHint) {
		showStartingHint = false;
		pthread_join(thrStartHint, NULL);
	}

	if(!res) {
		playback->Close();
	} else {
#if HAVE_SPARK_HARDWARE || HAVE_DUCKBOX_HARDWARE
		numpida = REC_MAX_APIDS;
		playback->FindAllPids(apids, ac3flags, &numpida, language);
		if (p_movie_info)
			for (int i = 0; i < numpida; i++) {
				unsigned int j, asize = p_movie_info->audioPids.size();
				for (j = 0; j < asize && p_movie_info->audioPids[j].epgAudioPid != apids[i]; j++);
				if (j == asize) {
					EPG_AUDIO_PIDS pids;
					pids.epgAudioPid = apids[i];
					pids.selected = 0;
					pids.atype = ac3flags[i];
					pids.epgAudioPidName = language[i];
					p_movie_info->audioPids.push_back(pids);
				}
			}
#endif
		playstate = CMoviePlayerGui::PLAY;
		CVFD::getInstance()->ShowIcon(FP_ICON_PLAY, true);
#if HAVE_DUCKBOX_HARDWARE || BOXMODEL_SPARK7162
		CVFD::getInstance()->ShowIcon(FP_ICON_FR, false);
		CVFD::getInstance()->ShowIcon(FP_ICON_FF, false);
		CVFD::getInstance()->ShowIcon(FP_ICON_PAUSE, false);
#endif
		if(timeshift) {
			startposition = -1;
			int i;
			int towait = (timeshift == 1) ? TIMESHIFT_SECONDS+1 : TIMESHIFT_SECONDS;
			for(i = 0; i < 500; i++) {
				playback->GetPosition(position, duration);
				startposition = (duration - position);

				//printf("CMoviePlayerGui::PlayFile: waiting for data, position %d duration %d (%d), start %d\n", position, duration, towait, startposition);
				if(startposition > towait*1000)
					break;

				usleep(20000);
			}
			if(timeshift == 3) {
				startposition = duration;
			} else {
				if(g_settings.timeshift_pause)
				{
					playstate = CMoviePlayerGui::PAUSE;
#if HAVE_DUCKBOX_HARDWARE || BOXMODEL_SPARK7162
					CVFD::getInstance()->ShowIcon(FP_ICON_PLAY, false);
					CVFD::getInstance()->ShowIcon(FP_ICON_FR, false);
					CVFD::getInstance()->ShowIcon(FP_ICON_FF, false);
					CVFD::getInstance()->ShowIcon(FP_ICON_PAUSE, true);
#endif
				}
				if(timeshift == 1)
					startposition = 0;
				else
					startposition = duration - TIMESHIFT_SECONDS*1000;
			}
			printf("******************* Timeshift %d, position %d, seek to %d seconds\n", timeshift, position, startposition/1000);
		}
		if(!is_file_player && startposition >= 0)//FIXME no jump for file at start yet
			playback->SetPosition(startposition, true);

		/* playback->Start() starts paused */
		if(timeshift == 3) {
			speed = -1;
			playback->SetSpeed(-1);
			playstate = CMoviePlayerGui::REW;
			if (!FileTime.IsVisible() && !time_forced) {
				FileTime.switchMode(position, duration);
				time_forced = true;
			}
		} else if(!timeshift || !g_settings.timeshift_pause) {
			playback->SetSpeed(1);
		}
	}
	if (is_file_player)
		selectAutoLang();

#if HAVE_DUCKBOX_HARDWARE || BOXMODEL_SPARK7162
	int xres, yres, framerate;
	CVFD::getInstance()->ShowIcon(FP_ICON_LOCK, false);
#endif
	CAudioMute::getInstance()->enableMuteIcon(true);
	InfoClock->enableInfoClock(true);

	while (playstate >= CMoviePlayerGui::PLAY)
	{
#ifdef ENABLE_GRAPHLCD
		if (p_movie_info && !isWebTV)
			nGLCD::lockChannel(p_movie_info->epgChannel, p_movie_info->epgTitle, duration ? (100 * position / duration) : 0);
		else {
			glcd_play = true;
			if (isWebTV)
				nGLCD::lockChannel(g_Locale->getText(LOCALE_WEBTV_HEAD), file_name.c_str(), file_prozent);
			else
				nGLCD::lockChannel(g_Locale->getText(LOCALE_MOVIEPLAYER_HEAD), file_name.c_str(), file_prozent);
		}
#endif
		if (update_lcd) {
			update_lcd = false;
			updateLcd();
		}
		if (first_start) {
			callInfoViewer(/*duration, position*/);
			first_start = false;
		}

		g_RCInput->getMsg(&msg, &data, 10);	// 1 secs..

		if ((playstate >= CMoviePlayerGui::PLAY) && (timeshift || (playstate != CMoviePlayerGui::PAUSE))) {
			if (isWebTV) {
				if (!playback->GetPosition(position, duration))
					g_RCInput->postMsg((neutrino_msg_t) g_settings.mpkey_stop, 0);
			} else {
			if(playback->GetPosition(position, duration)) {
				FileTime.update(position, duration);
				if(duration > 100)
					file_prozent = (unsigned char) (position / (duration / 100));
#if HAVE_TRIPLEDRAGON
				CVFD::getInstance()->showPercentOver(file_prozent, true, CVFD::MODE_MOVIE);
#else
				CVFD::getInstance()->showPercentOver(file_prozent);
#endif

				playback->GetSpeed(speed);
				/* at BOF lib set speed 1, check it */
				if ((playstate != CMoviePlayerGui::PLAY) && (speed == 1)) {
					playstate = CMoviePlayerGui::PLAY;
					update_lcd = true;
				}
#ifdef DEBUG
				printf("CMoviePlayerGui::PlayFile: speed %d position %d duration %d (%d, %d%%)\n", speed, position, duration, duration-position, file_prozent);
#endif
				/* in case ffmpeg report incorrect values */
#if HAVE_DUCKBOX_HARDWARE || BOXMODEL_SPARK7162
					if ((position > 100) && (file_prozent < 3))
				{
					/* 10 seconds after end-of-file, stop */
						videoDecoder->getPictureInfo(xres, yres, framerate);
						CVFD::getInstance()->ShowIcon(FP_ICON_HD, (yres > 576));
					}
#endif
				} else
						g_RCInput->postMsg((neutrino_msg_t) g_settings.mpkey_stop, 0);
				}
			handleMovieBrowser(0, position);
			FileTime.update(position, duration);
		}
		showSubtitle(0);

		if (msg == (neutrino_msg_t) g_settings.mpkey_plugin) {
			g_PluginList->startPlugin_by_name(g_settings.movieplayer_plugin.c_str ());
		} else if (msg == (neutrino_msg_t) g_settings.mpkey_stop || (filelist.size() > 0 && msg == (neutrino_msg_t) CRCInput::RC_right)) {
			bool stop_it = true;
			if ((timeshift) && (g_settings.temp_timeshift))
				stop_it = (ShowMsg(LOCALE_RECORDINGMENU_MULTIMENU_TIMESHIFT, LOCALE_RECORDINGMENU_MULTIMENU_TIMESHIFT_STOP, CMessageBox::mbrYes, CMessageBox::mbYes | CMessageBox::mbNo, NULL, 450, 30, false) == CMessageBox::mbrYes);
			if (stop_it)
				playstate = CMoviePlayerGui::STOPPED;
			playback->RequestAbort();
			if (filelist.size() > 0) {
				if (filelist_it == filelist.end() && repeat_mode == REPEAT_ALL)
					filelist_it = filelist.begin();
				else if (repeat_mode == REPEAT_TRACK)
					--filelist_it;

				if (filelist_it == filelist.end())
					repeat_mode = REPEAT_OFF;
				else {
					is_file_player = true;
					full_name = (*filelist_it).Name;
					std::string::size_type pos = full_name.find_last_of('/');
					if(pos != std::string::npos) {
						file_name = full_name.substr(pos+1);
						std::replace(file_name.begin(), file_name.end(), '_', ' ');
					} else
						file_name = full_name;
				}
			}
		} else if (msg == (neutrino_msg_t) CRCInput::RC_home) {
			playstate = CMoviePlayerGui::STOPPED;
			playback->RequestAbort();
			filelist.clear();
			repeat_mode = REPEAT_OFF;
#if HAVE_SPARK_HARDWARE || HAVE_DUCKBOX_HARDWARE
		} else if (msg == (neutrino_msg_t) g_settings.mpkey_next3dmode) {
			frameBuffer->set3DMode((CFrameBuffer::Mode3D)(((frameBuffer->get3DMode()) + 1) % CFrameBuffer::Mode3D_SIZE));
#endif
		} else if(filelist.size() > 1 && msg == (neutrino_msg_t) CRCInput::RC_left) {
			if (filelist_it != filelist.begin())
				--filelist_it;
			if (filelist_it == filelist.begin())
				filelist_it = filelist.end();
			--filelist_it;
			playstate = CMoviePlayerGui::STOPPED;
			playback->RequestAbort();
		} else if(!timeshift && !isWebTV && (msg == (neutrino_msg_t) g_settings.mpkey_next_repeat_mode)) {
			repeat_mode = (repeat_mode_enum)((int)repeat_mode + 1);
			if (repeat_mode > (int) REPEAT_ALL)
				repeat_mode = REPEAT_OFF;
			callInfoViewer();
#if HAVE_SPARK_HARDWARE || HAVE_DUCKBOX_HARDWARE
		} else if( msg == (neutrino_msg_t) g_settings.key_next43mode) {
			g_videoSettings->next43Mode();
		} else if( msg == (neutrino_msg_t) g_settings.key_switchformat) {
			g_videoSettings->SwitchFormat();
#endif
			if ((duration - position) > 600000)
				makeScreenShot(true);
		} else if (msg == (neutrino_msg_t) g_settings.mpkey_play) {
			if (time_forced) {
				time_forced = false;
				FileTime.kill();
			}
			if (playstate > CMoviePlayerGui::PLAY) {
				playstate = CMoviePlayerGui::PLAY;
#if HAVE_DUCKBOX_HARDWARE || BOXMODEL_SPARK7162
				CVFD::getInstance()->ShowIcon(FP_ICON_PLAY, true);
				CVFD::getInstance()->ShowIcon(FP_ICON_PAUSE, false);
				CVFD::getInstance()->ShowIcon(FP_ICON_FR, false);
				CVFD::getInstance()->ShowIcon(FP_ICON_FF, false);
#endif
				speed = 1;
				playback->SetSpeed(speed);
				//update_lcd = true;
				updateLcd();
				if (!timeshift)
					callInfoViewer(/*duration, position*/);
			}
		} else if (msg == (neutrino_msg_t) g_settings.mpkey_pause) {
			if (playstate == CMoviePlayerGui::PAUSE) {
				playstate = CMoviePlayerGui::PLAY;
				//CVFD::getInstance()->ShowIcon(VFD_ICON_PAUSE, false);
#if HAVE_DUCKBOX_HARDWARE || BOXMODEL_SPARK7162
				CVFD::getInstance()->ShowIcon(FP_ICON_PLAY, true);
				CVFD::getInstance()->ShowIcon(FP_ICON_PAUSE, false);
				CVFD::getInstance()->ShowIcon(FP_ICON_FR, false);
				CVFD::getInstance()->ShowIcon(FP_ICON_FF, false);
#endif
				speed = 1;
				playback->SetSpeed(speed);
			} else {
				playstate = CMoviePlayerGui::PAUSE;
				//CVFD::getInstance()->ShowIcon(VFD_ICON_PAUSE, true);
#if HAVE_DUCKBOX_HARDWARE || BOXMODEL_SPARK7162
				CVFD::getInstance()->ShowIcon(FP_ICON_PLAY, false);
				CVFD::getInstance()->ShowIcon(FP_ICON_PAUSE, true);
				CVFD::getInstance()->ShowIcon(FP_ICON_FR, false);
				CVFD::getInstance()->ShowIcon(FP_ICON_FF, false);
#endif
				speed = 0;
				playback->SetSpeed(speed);
			}
			//update_lcd = true;
			updateLcd();
			if (!timeshift)
				callInfoViewer(/*duration, position*/);

		} else if (msg == (neutrino_msg_t) g_settings.mpkey_bookmark) {
			if (is_file_player)
				selectChapter();
			else
				handleMovieBrowser((neutrino_msg_t) g_settings.mpkey_bookmark, position);
			update_lcd = true;
			clearSubtitle();
		} else if (msg == (neutrino_msg_t) g_settings.mpkey_audio) {
			selectAudioPid(is_file_player);
			update_lcd = true;
			clearSubtitle();
		} else if ( msg == (neutrino_msg_t) g_settings.mpkey_subtitle) {
			selectSubtitle();
			clearSubtitle();
			update_lcd = true;
		} else if (msg == (neutrino_msg_t) g_settings.mpkey_time) {
			FileTime.switchMode(position, duration);
		} else if (/*!is_file_player &&*/ ((msg == (neutrino_msg_t) g_settings.mpkey_rewind) ||
				(msg == (neutrino_msg_t) g_settings.mpkey_forward))) {

			int newspeed;
			if (msg == (neutrino_msg_t) g_settings.mpkey_rewind) {
				newspeed = (speed >= 0) ? -1 : (speed - 1);
#if HAVE_DUCKBOX_HARDWARE || BOXMODEL_SPARK7162
				CVFD::getInstance()->ShowIcon(FP_ICON_PLAY, true);
				CVFD::getInstance()->ShowIcon(FP_ICON_PAUSE, false);
				CVFD::getInstance()->ShowIcon(FP_ICON_FR, true);
				CVFD::getInstance()->ShowIcon(FP_ICON_FF, false);
#endif
			} else {
				newspeed = (speed <= 0) ? 2 : (speed + 1);
#if HAVE_DUCKBOX_HARDWARE || BOXMODEL_SPARK7162
				CVFD::getInstance()->ShowIcon(FP_ICON_PLAY, true);
				CVFD::getInstance()->ShowIcon(FP_ICON_PAUSE, false);
				CVFD::getInstance()->ShowIcon(FP_ICON_FR, false);
				CVFD::getInstance()->ShowIcon(FP_ICON_FF, true);
#endif
			}
			/* if paused, playback->SetSpeed() start slow motion */
			if (playback->SetSpeed(newspeed)) {
				printf("SetSpeed: update speed\n");
				speed = newspeed;
				if (playstate != CMoviePlayerGui::PAUSE)
					playstate = msg == (neutrino_msg_t) g_settings.mpkey_rewind ? CMoviePlayerGui::REW : CMoviePlayerGui::FF;
				updateLcd();
			}
			//update_lcd = true;

			if (!FileTime.IsVisible() && !time_forced) {
				FileTime.switchMode(position, duration);
				time_forced = true;
			}
			if (!timeshift)
				callInfoViewer(/*duration, position*/);
		} else if (msg == CRCInput::RC_1) {	// Jump Backwards 1 minute
			clearSubtitle();
			playback->SetPosition(-60 * 1000);
		} else if (msg == CRCInput::RC_3) {	// Jump Forward 1 minute
			clearSubtitle();
			playback->SetPosition(60 * 1000);
		} else if (msg == CRCInput::RC_4) {	// Jump Backwards 5 minutes
			clearSubtitle();
			playback->SetPosition(-5 * 60 * 1000);
		} else if (msg == CRCInput::RC_6) {	// Jump Forward 5 minutes
			clearSubtitle();
			playback->SetPosition(5 * 60 * 1000);
		} else if (msg == CRCInput::RC_7) {	// Jump Backwards 10 minutes
			clearSubtitle();
			playback->SetPosition(-10 * 60 * 1000);
		} else if (msg == CRCInput::RC_9) {	// Jump Forward 10 minutes
			clearSubtitle();
			playback->SetPosition(10 * 60 * 1000);
		} else if (msg == CRCInput::RC_2) {	// goto start
			clearSubtitle();
			playback->SetPosition(0, true);
		} else if (msg == CRCInput::RC_5) {	// goto middle
			clearSubtitle();
			playback->SetPosition(duration/2, true);
		} else if (msg == CRCInput::RC_8) {	// goto end
			clearSubtitle();
			playback->SetPosition(duration - 60 * 1000, true);
		} else if (msg == CRCInput::RC_page_up) {
			clearSubtitle();
			playback->SetPosition(10 * 1000);
		} else if (msg == CRCInput::RC_page_down) {
			clearSubtitle();
			playback->SetPosition(-10 * 1000);
		} else if (msg == CRCInput::RC_0) {	// cancel bookmark jump
			handleMovieBrowser(CRCInput::RC_0, position);
#if HAVE_SPARK_HARDWARE || HAVE_DUCKBOX_HARDWARE
		} else if (msg == (neutrino_msg_t) g_settings.mpkey_goto) {
			bool cancel = true;
			playback->GetPosition(position, duration);
			int ss = position/1000;
			int hh = ss/3600;
			ss -= hh * 3600;
			int mm = ss/60;
			ss -= mm * 60;
			std::string Value = to_string(hh/10) + to_string(hh%10) + ":" + to_string(mm/10) + to_string(mm%10) + ":" + to_string(ss/10) + to_string(ss%10);
			CTimeInput jumpTime (LOCALE_MPKEY_GOTO, &Value, NONEXISTANT_LOCALE, NONEXISTANT_LOCALE, NULL, &cancel);
			jumpTime.exec(NULL, "");
			jumpTime.hide();
			if (!cancel && (3 == sscanf(Value.c_str(), "%d:%d:%d", &hh, &mm, &ss)))
				playback->SetPosition(1000 * (hh * 3600 + mm * 60 + ss), true);
#endif
		} else if (msg == CRCInput::RC_help || msg == CRCInput::RC_info) {
			callInfoViewer(/*duration, position*/);
			update_lcd = true;
			clearSubtitle();
			//showHelpTS();
		} else if(timeshift && (msg == CRCInput::RC_text || msg == CRCInput::RC_epg || msg == NeutrinoMessages::SHOW_EPG)) {
			bool restore = FileTime.IsVisible();
			FileTime.kill();

			if( msg == CRCInput::RC_epg )
				g_EventList->exec(CNeutrinoApp::getInstance()->channelList->getActiveChannel_ChannelID(), CNeutrinoApp::getInstance()->channelList->getActiveChannelName());
			else if(msg == NeutrinoMessages::SHOW_EPG)
				g_EpgData->show(CNeutrinoApp::getInstance()->channelList->getActiveChannel_ChannelID());
			else {
				if(g_settings.cacheTXT)
					tuxtxt_stop();
				tuxtx_main(g_RCInput->getFileHandle(), g_RemoteControl->current_PIDs.PIDs.vtxtpid, 0, 2);
				frameBuffer->paintBackground();
			}
			if(restore)
				FileTime.show(position);
		} else if (msg == NeutrinoMessages::SHOW_EPG) {
			handleMovieBrowser(NeutrinoMessages::SHOW_EPG, position);
		} else if (msg == (neutrino_msg_t) g_settings.key_screenshot) {
			makeScreenShot();
		} else if ( msg == NeutrinoMessages::EVT_SUBT_MESSAGE) {
			showSubtitle(data);
		} else if ( msg == NeutrinoMessages::ANNOUNCE_RECORD ||
				msg == NeutrinoMessages::RECORD_START) {
			CNeutrinoApp::getInstance()->handleMsg(msg, data);
		} else if ( msg == NeutrinoMessages::ZAPTO ||
				msg == NeutrinoMessages::STANDBY_ON ||
				msg == NeutrinoMessages::SHUTDOWN ||
				((msg == NeutrinoMessages::SLEEPTIMER) && !data) ) {	// Exit for Record/Zapto Timers
			printf("CMoviePlayerGui::PlayFile: ZAPTO etc..\n");
			if(msg != NeutrinoMessages::ZAPTO)
				menu_ret = menu_return::RETURN_EXIT_ALL;

			playstate = CMoviePlayerGui::STOPPED;
			g_RCInput->postMsg(msg, data);
		} else if (msg == CRCInput::RC_timeout || msg == NeutrinoMessages::EVT_TIMER) {
			if (playstate == CMoviePlayerGui::PLAY && (position >= 300000 || (duration<300000 && (position>(duration /2)))))
				makeScreenShot(true);
		} else if (msg == CRCInput::RC_favorites) {
			makeScreenShot(false, true);
		} else if (msg == CRCInput::RC_sat) {
			//FIXME do nothing ?
		} else {
			if (CNeutrinoApp::getInstance()->handleMsg(msg, data) & messages_return::cancel_all) {
				printf("CMoviePlayerGui::PlayFile: neutrino handleMsg messages_return::cancel_all\n");
				playstate = CMoviePlayerGui::STOPPED;
				menu_ret = menu_return::RETURN_EXIT_ALL;
				repeat_mode = REPEAT_OFF;
			}
			else if ( msg <= CRCInput::RC_MaxRC ) {
				update_lcd = true;
				clearSubtitle();
			}
		}

		if (playstate == CMoviePlayerGui::STOPPED) {
			printf("CMoviePlayerGui::PlayFile: exit, isMovieBrowser %d p_movie_info %p\n", isMovieBrowser, p_movie_info);
			playstate = CMoviePlayerGui::STOPPED;
			handleMovieBrowser((neutrino_msg_t) g_settings.mpkey_stop, position);
		}
	}
#if HAVE_SPARK_HARDWARE || HAVE_DUCKBOX_HARDWARE
	frameBuffer->set3DMode(old3dmode);
#ifdef ENABLE_GRAPHLCD
	if (p_movie_info || glcd_play == true) {
		glcd_play = false;
		nGLCD::unlockChannel();
	}
#endif
#endif

	FileTime.kill();
	clearSubtitle();

	playback->SetSpeed(1);
	playback->Close();
	if (iso_file) {
		iso_file = false;
		if (umount2(ISO_MOUNT_POINT ,MNT_FORCE))
			perror(ISO_MOUNT_POINT);
	}

	CVFD::getInstance()->ShowIcon(FP_ICON_PLAY, false);
	CVFD::getInstance()->ShowIcon(FP_ICON_PAUSE, false);
#if HAVE_DUCKBOX_HARDWARE || BOXMODEL_SPARK7162
	CVFD::getInstance()->ShowIcon(FP_ICON_FR, false);
	CVFD::getInstance()->ShowIcon(FP_ICON_FF, false);
#endif

	restoreNeutrino();

	CAudioMute::getInstance()->enableMuteIcon(false);
	InfoClock->enableInfoClock(false);
}

void CMoviePlayerGui::callInfoViewer(/*const int duration, const int curr_pos*/)
{
	if(timeshift) {
		g_InfoViewer->showTitle(CNeutrinoApp::getInstance()->channelList->getActiveChannelNumber(),
				CNeutrinoApp::getInstance()->channelList->getActiveChannelName(),
				CNeutrinoApp::getInstance()->channelList->getActiveSatellitePosition(),
				CNeutrinoApp::getInstance()->channelList->getActiveChannel_ChannelID());
		return;
	}
	currentaudioname = "Unk";
	getCurrentAudioName( is_file_player, currentaudioname);

	if (isMovieBrowser && p_movie_info) {
		g_InfoViewer->showMovieTitle(playstate, p_movie_info->epgEpgId >>16, p_movie_info->epgChannel, p_movie_info->epgTitle, p_movie_info->epgInfo1,
					     duration, position , repeat_mode);
		return;
	}

	/* not moviebrowser => use the filename as title */
	g_InfoViewer->showMovieTitle(playstate, 0, file_name, "", "", duration, position, repeat_mode);
}

bool CMoviePlayerGui::getAudioName(int apid, std::string &apidtitle)
{
	if (p_movie_info == NULL)
		return false;

	for (int i = 0; i < (int)p_movie_info->audioPids.size(); i++) {
		if (p_movie_info->audioPids[i].epgAudioPid == apid && !p_movie_info->audioPids[i].epgAudioPidName.empty()) {
			apidtitle = p_movie_info->audioPids[i].epgAudioPidName;
			return true;
		}
	}
	return false;
}

void CMoviePlayerGui::addAudioFormat(int count, std::string &apidtitle, bool& enabled)
{
	enabled = true;
	switch(ac3flags[count])
	{
		case 1: /*AC3,EAC3*/
			if (apidtitle.find("AC3") == std::string::npos)
				apidtitle.append(" (AC3)");
			break;
		case 2: /*teletext*/
			apidtitle.append(" (Teletext)");
			enabled = false;
			break;
		case 3: /*MP2*/
			apidtitle.append("( MP2)");
			break;
		case 4: /*MP3*/
			apidtitle.append(" (MP3)");
			break;
		case 5: /*AAC*/
			apidtitle.append(" (AAC)");
			break;
		case 6: /*DTS*/
#if !defined (BOXMODEL_APOLLO)
			if (apidtitle.find("DTS") == std::string::npos)
				apidtitle.append(" (DTS)");
			else
#endif
			enabled = false;
			break;
		case 7: /*MLP*/
			apidtitle.append(" (MLP)");
			break;
		default:
			break;
	}
}

void CMoviePlayerGui::getCurrentAudioName( bool file_player, std::string &audioname)
{
	if(file_player && !numpida){
		playback->FindAllPids(apids, ac3flags, &numpida, language);
		if(numpida)
			currentapid = apids[0];
	}
	bool dumm = true;
	for (unsigned int count = 0; count < numpida; count++) {
		if(currentapid == apids[count]){
			if(!file_player){
				getAudioName(apids[count], audioname);
				return ;
			} else if (!language[count].empty()){
				audioname = language[count];
				addAudioFormat(count, audioname, dumm);
				if(!dumm && (count < numpida)){
					currentapid = apids[count+1];
					continue;
				}
				return ;
			}
			char apidnumber[20];
			sprintf(apidnumber, "Stream %d %X", count + 1, apids[count]);
			audioname = apidnumber;
			addAudioFormat(count, audioname, dumm);
			if(!dumm && (count < numpida)){
				currentapid = apids[count+1];
				continue;
			}
			return ;
		}
	}
}

void CMoviePlayerGui::selectAudioPid(bool file_player)
{
	CMenuWidget APIDSelector(LOCALE_APIDSELECTOR_HEAD, NEUTRINO_ICON_AUDIO);
	APIDSelector.addIntroItems();

	int select = -1;
	CMenuSelectorTarget * selector = new CMenuSelectorTarget(&select);

	if(file_player && !numpida){
		playback->FindAllPids(apids, ac3flags, &numpida, language);
		if(numpida)
			currentapid = apids[0];
	}
	for (unsigned int count = 0; count < numpida; count++) {
		bool name_ok = false;
		bool enabled = true;
		bool defpid = currentapid ? (currentapid == apids[count]) : (count == 0);
		std::string apidtitle;

		if(!file_player){
			name_ok = getAudioName(apids[count], apidtitle);
		}
		else if (!language[count].empty()){
			apidtitle = language[count];
			name_ok = true;
		}
		if (!name_ok) {
			char apidnumber[20];
			sprintf(apidnumber, "Stream %d %X", count + 1, apids[count]);
			apidtitle = apidnumber;
		}
		addAudioFormat(count, apidtitle, enabled);
		if(defpid && !enabled && (count < numpida)){
			currentapid = apids[count+1];
			defpid = false;
		}

		char cnt[5];
		sprintf(cnt, "%d", count);
		CMenuForwarder * item = new CMenuForwarder(apidtitle.c_str(), enabled, NULL, selector, cnt, CRCInput::convertDigitToKey(count + 1));
		APIDSelector.addItem(item, defpid);
	}

	if (p_movie_info && numpida <= p_movie_info->audioPids.size()) {
		APIDSelector.addItem(new CMenuSeparator(CMenuSeparator::LINE | CMenuSeparator::STRING, LOCALE_AUDIOMENU_VOLUME_ADJUST));

		CVolume::getInstance()->SetCurrentChannel(p_movie_info->epgId);
		CVolume::getInstance()->SetCurrentPid(currentapid);
		int percent[numpida];
		for (uint i=0; i < numpida; i++) {
			percent[i] = CZapit::getInstance()->GetPidVolume(p_movie_info->epgId, apids[i], ac3flags[i]);
			APIDSelector.addItem(new CMenuOptionNumberChooser(p_movie_info->audioPids[i].epgAudioPidName,
						&percent[i], currentapid == apids[i],
						0, 999, CVolume::getInstance()));
		}
	}

	APIDSelector.exec(NULL, "");
	delete selector;
	printf("CMoviePlayerGui::selectAudioPid: selected %d (%x) current %x\n", select, (select >= 0) ? apids[select] : -1, currentapid);
	if((select >= 0) && (currentapid != apids[select])) {
		currentapid = apids[select];
		currentac3 = ac3flags[select];
		playback->SetAPid(currentapid, currentac3);
#if HAVE_DUCKBOX_HARDWARE || BOXMODEL_SPARK7162
		CVFD::getInstance()->ShowIcon(FP_ICON_DD, currentac3);
#endif
		printf("[movieplayer] apid changed to %d type %d\n", currentapid, currentac3);
	}
}

void CMoviePlayerGui::handleMovieBrowser(neutrino_msg_t msg, int /*position*/)
{
	CMovieInfo cMovieInfo;	// funktions to save and load movie info

	static int jump_not_until = 0;	// any jump shall be avoided until this time (in seconds from moviestart)
	static MI_BOOKMARK new_bookmark;	// used for new movie info bookmarks created from the movieplayer

	static int width = 280;
	static int height = 65;

	static int x = frameBuffer->getScreenX() + (frameBuffer->getScreenWidth() - width) / 2;
	static int y = frameBuffer->getScreenY() + frameBuffer->getScreenHeight() - height - 20;

	static CBox boxposition(x, y, width, height);	// window position for the hint boxes

	static CTextBox endHintBox(g_Locale->getText(LOCALE_MOVIEBROWSER_HINT_MOVIEEND), NULL, CTextBox::CENTER /*CTextBox::AUTO_WIDTH | CTextBox::AUTO_HIGH */ , &boxposition);
	static CTextBox comHintBox(g_Locale->getText(LOCALE_MOVIEBROWSER_HINT_JUMPFORWARD), NULL, CTextBox::CENTER /*CTextBox::AUTO_WIDTH | CTextBox::AUTO_HIGH */ , &boxposition);
	static CTextBox loopHintBox(g_Locale->getText(LOCALE_MOVIEBROWSER_HINT_JUMPBACKWARD), NULL, CTextBox::CENTER /*CTextBox::AUTO_WIDTH | CTextBox::AUTO_HIGH */ , &boxposition);
	static CTextBox newLoopHintBox(g_Locale->getText(LOCALE_MOVIEBROWSER_HINT_NEWBOOK_BACKWARD), NULL, CTextBox::CENTER /*CTextBox::AUTO_WIDTH | CTextBox::AUTO_HIGH */ , &boxposition);
	static CTextBox newComHintBox(g_Locale->getText(LOCALE_MOVIEBROWSER_HINT_NEWBOOK_FORWARD), NULL, CTextBox::CENTER /*CTextBox::AUTO_WIDTH | CTextBox::AUTO_HIGH */ , &boxposition);

	static bool showEndHintBox = false;	// flag to check whether the box shall be painted
	static bool showComHintBox = false;	// flag to check whether the box shall be painted
	static bool showLoopHintBox = false;	// flag to check whether the box shall be painted

	int play_sec = position / 1000;	// get current seconds from moviestart
	if(msg == CRCInput::RC_nokey) {
		printf("CMoviePlayerGui::handleMovieBrowser: reset vars\n");
		// reset statics
		jump_not_until = 0;
		showEndHintBox = showComHintBox = showLoopHintBox = false;
		new_bookmark.pos = 0;
		// move in case osd position changed
		int newx = frameBuffer->getScreenX() + (frameBuffer->getScreenWidth() - width) / 2;
		int newy = frameBuffer->getScreenY() + frameBuffer->getScreenHeight() - height - 20;
		endHintBox.movePosition(newx, newy);
		comHintBox.movePosition(newx, newy);
		loopHintBox.movePosition(newx, newy);
		newLoopHintBox.movePosition(newx, newy);
		newComHintBox.movePosition(newx, newy);
		return;
	}
	else if (msg == (neutrino_msg_t) g_settings.mpkey_stop) {
		// if we have a movie information, try to save the stop position
		printf("CMoviePlayerGui::handleMovieBrowser: stop, isMovieBrowser %d p_movie_info %p\n", isMovieBrowser, p_movie_info);
		if (isMovieBrowser && p_movie_info) {
			timeb current_time;
			ftime(&current_time);
			p_movie_info->dateOfLastPlay = current_time.time;
			current_time.time = time(NULL);
			p_movie_info->bookmarks.lastPlayStop = position / 1000;
			if (!isWebTV)
			cMovieInfo.saveMovieInfo(*p_movie_info);
			//p_movie_info->fileInfoStale(); //TODO: we might to tell the Moviebrowser that the movie info has changed, but this could cause long reload times  when reentering the Moviebrowser
		}
	}
	else if((msg == 0) && isMovieBrowser && (playstate == CMoviePlayerGui::PLAY) && p_movie_info) {
		if (play_sec + 10 < jump_not_until || play_sec > jump_not_until + 10)
			jump_not_until = 0;	// check if !jump is stale (e.g. if user jumped forward or backward)

		// do bookmark activities only, if there is no new bookmark started
		if (new_bookmark.pos != 0)
			return;
#ifdef DEBUG
		//printf("CMoviePlayerGui::handleMovieBrowser: process bookmarks\n");
#endif
		if (p_movie_info->bookmarks.end != 0) {
			// *********** Check for stop position *******************************
			if (play_sec >= p_movie_info->bookmarks.end - MOVIE_HINT_BOX_TIMER && play_sec < p_movie_info->bookmarks.end && play_sec > jump_not_until) {
				if (showEndHintBox == false) {
					endHintBox.paint();	// we are 5 sec before the end postition, show warning
					showEndHintBox = true;
					TRACE("[mp]  user stop in 5 sec...\r\n");
				}
			} else {
				if (showEndHintBox == true) {
					endHintBox.hide();	// if we showed the warning before, hide the box again
					showEndHintBox = false;
				}
			}

			if (play_sec >= p_movie_info->bookmarks.end && play_sec <= p_movie_info->bookmarks.end + 2 && play_sec > jump_not_until)	// stop playing
			{
				// *********** we ARE close behind the stop position, stop playing *******************************
				TRACE("[mp]  user stop: play_sec %d bookmarks.end %d jump_not_until %d\n", play_sec, p_movie_info->bookmarks.end, jump_not_until);
				playstate = CMoviePlayerGui::STOPPED;
				return;
			}
		}
		// *************  Check for bookmark jumps *******************************
		showLoopHintBox = false;
		showComHintBox = false;
		for (int book_nr = 0; book_nr < MI_MOVIE_BOOK_USER_MAX; book_nr++) {
			if (p_movie_info->bookmarks.user[book_nr].pos != 0 && p_movie_info->bookmarks.user[book_nr].length != 0) {
				// valid bookmark found, now check if we are close before or after it
				if (play_sec >= p_movie_info->bookmarks.user[book_nr].pos - MOVIE_HINT_BOX_TIMER && play_sec < p_movie_info->bookmarks.user[book_nr].pos && play_sec > jump_not_until) {
					if (p_movie_info->bookmarks.user[book_nr].length < 0)
						showLoopHintBox = true;	// we are 5 sec before , show warning
					else if (p_movie_info->bookmarks.user[book_nr].length > 0)
						showComHintBox = true;	// we are 5 sec before, show warning
					//else  // TODO should we show a plain bookmark infomation as well?
				}

				if (play_sec >= p_movie_info->bookmarks.user[book_nr].pos && play_sec <= p_movie_info->bookmarks.user[book_nr].pos + 2 && play_sec > jump_not_until)	//
				{
					//for plain bookmark, the following calc shall result in 0 (no jump)
					int jumpseconds = p_movie_info->bookmarks.user[book_nr].length;

					// we are close behind the bookmark, do bookmark activity (if any)
					if (p_movie_info->bookmarks.user[book_nr].length < 0) {
						// if the jump back time is to less, it does sometimes cause problems (it does probably jump only 5 sec which will cause the next jump, and so on)
						if (jumpseconds > -15)
							jumpseconds = -15;

						playback->SetPosition(jumpseconds * 1000);
					} else if (p_movie_info->bookmarks.user[book_nr].length > 0) {
						// jump at least 15 seconds
						if (jumpseconds < 15)
							jumpseconds = 15;

						playback->SetPosition(jumpseconds * 1000);
					}
					TRACE("[mp]  do jump %d sec\r\n", jumpseconds);
					break;	// do no further bookmark checks
				}
			}
		}
		// check if we shall show the commercial warning
		if (showComHintBox == true) {
			comHintBox.paint();
			TRACE("[mp]  com jump in 5 sec...\r\n");
		} else
			comHintBox.hide();

		// check if we shall show the loop warning
		if (showLoopHintBox == true) {
			loopHintBox.paint();
			TRACE("[mp]  loop jump in 5 sec...\r\n");
		} else
			loopHintBox.hide();

		return;
	} else if (msg == CRCInput::RC_0) {	// cancel bookmark jump
		printf("CMoviePlayerGui::handleMovieBrowser: CRCInput::RC_0\n");
		if (isMovieBrowser == true) {
			if (new_bookmark.pos != 0) {
				new_bookmark.pos = 0;	// stop current bookmark activity, TODO:  might bemoved to another key
				newLoopHintBox.hide();	// hide hint box if any
				newComHintBox.hide();
			}
			comHintBox.hide();
			loopHintBox.hide();
			jump_not_until = (position / 1000) + 10; // avoid bookmark jumping for the next 10 seconds, , TODO:  might be moved to another key
		}
		return;
	}
	else if (msg == (neutrino_msg_t) g_settings.mpkey_bookmark && !isWebTV) {
		if (newComHintBox.isPainted() == true) {
			// yes, let's get the end pos of the jump forward
			new_bookmark.length = play_sec - new_bookmark.pos;
			TRACE("[mp] commercial length: %d\r\n", new_bookmark.length);
			if (cMovieInfo.addNewBookmark(p_movie_info, new_bookmark) == true) {
				cMovieInfo.saveMovieInfo(*p_movie_info);	/* save immediately in xml file */
			}
			new_bookmark.pos = 0;	// clear again, since this is used as flag for bookmark activity
			newComHintBox.hide();
		} else if (newLoopHintBox.isPainted() == true) {
			// yes, let's get the end pos of the jump backward
			new_bookmark.length = new_bookmark.pos - play_sec;
			new_bookmark.pos = play_sec;
			TRACE("[mp] loop length: %d\r\n", new_bookmark.length);
			if (cMovieInfo.addNewBookmark(p_movie_info, new_bookmark) == true) {
				cMovieInfo.saveMovieInfo(*p_movie_info);	/* save immediately in xml file */
				jump_not_until = play_sec + 5;	// avoid jumping for this time
			}
			new_bookmark.pos = 0;	// clear again, since this is used as flag for bookmark activity
			newLoopHintBox.hide();
		} else {
			// very dirty usage of the menue, but it works and I already spent to much time with it, feel free to make it better ;-)
#define BOOKMARK_START_MENU_MAX_ITEMS 6
			CSelectedMenu cSelectedMenuBookStart[BOOKMARK_START_MENU_MAX_ITEMS];

			CMenuWidget bookStartMenu(LOCALE_MOVIEBROWSER_BOOK_ADD, NEUTRINO_ICON_STREAMING);
			bookStartMenu.addIntroItems();
#if 0 // not supported, TODO
			bookStartMenu.addItem(new CMenuForwarder(LOCALE_MOVIEPLAYER_HEAD, !isMovieBrowser, NULL, &cSelectedMenuBookStart[0]));
			bookStartMenu.addItem(GenericMenuSeparatorLine);
#endif
			bookStartMenu.addItem(new CMenuForwarder(LOCALE_MOVIEBROWSER_BOOK_NEW, isMovieBrowser, NULL, &cSelectedMenuBookStart[1]));
			bookStartMenu.addItem(new CMenuForwarder(LOCALE_MOVIEBROWSER_BOOK_TYPE_FORWARD, isMovieBrowser, NULL, &cSelectedMenuBookStart[2]));
			bookStartMenu.addItem(new CMenuForwarder(LOCALE_MOVIEBROWSER_BOOK_TYPE_BACKWARD, isMovieBrowser, NULL, &cSelectedMenuBookStart[3]));
			bookStartMenu.addItem(new CMenuForwarder(LOCALE_MOVIEBROWSER_BOOK_MOVIESTART, isMovieBrowser, NULL, &cSelectedMenuBookStart[4]));
			bookStartMenu.addItem(new CMenuForwarder(LOCALE_MOVIEBROWSER_BOOK_MOVIEEND, isMovieBrowser, NULL, &cSelectedMenuBookStart[5]));

			// no, nothing else to do, we open a new bookmark menu
			new_bookmark.name = "";	// use default name
			new_bookmark.pos = 0;
			new_bookmark.length = 0;

			// next seems return menu_return::RETURN_EXIT, if something selected
			bookStartMenu.exec(NULL, "none");
#if 0 // not supported, TODO
			if (cSelectedMenuBookStart[0].selected == true) {
				/* Movieplayer bookmark */
				if (bookmarkmanager->getBookmarkCount() < bookmarkmanager->getMaxBookmarkCount()) {
					char timerstring[200];
					sprintf(timerstring, "%lld", play_sec);
					std::string bookmarktime = timerstring;
					fprintf(stderr, "fileposition: %lld timerstring: %s bookmarktime: %s\n", play_sec, timerstring, bookmarktime.c_str());
					bookmarkmanager->createBookmark(full_name, bookmarktime);
				} else {
					fprintf(stderr, "too many bookmarks\n");
					DisplayErrorMessage(g_Locale->getText(LOCALE_MOVIEPLAYER_TOOMANYBOOKMARKS));	// UTF-8
				}
				cSelectedMenuBookStart[0].selected = false;	// clear for next bookmark menu
			} else
#endif
			if (cSelectedMenuBookStart[1].selected == true) {
				/* Moviebrowser plain bookmark */
				new_bookmark.pos = play_sec;
				new_bookmark.length = 0;
				if (!isWebTV && cMovieInfo.addNewBookmark(p_movie_info, new_bookmark) == true)
					cMovieInfo.saveMovieInfo(*p_movie_info);	/* save immediately in xml file */
				new_bookmark.pos = 0;	// clear again, since this is used as flag for bookmark activity
				cSelectedMenuBookStart[1].selected = false;	// clear for next bookmark menu
			} else if (cSelectedMenuBookStart[2].selected == true) {
				/* Moviebrowser jump forward bookmark */
				new_bookmark.pos = play_sec;
				TRACE("[mp] new bookmark 1. pos: %d\r\n", new_bookmark.pos);
				newComHintBox.paint();
				cSelectedMenuBookStart[2].selected = false;	// clear for next bookmark menu
			} else if (cSelectedMenuBookStart[3].selected == true) {
				/* Moviebrowser jump backward bookmark */
				new_bookmark.pos = play_sec;
				TRACE("[mp] new bookmark 1. pos: %d\r\n", new_bookmark.pos);
				newLoopHintBox.paint();
				cSelectedMenuBookStart[3].selected = false;	// clear for next bookmark menu
			} else if (!isWebTV && cSelectedMenuBookStart[4].selected == true) {
				/* Moviebrowser movie start bookmark */
				p_movie_info->bookmarks.start = play_sec;
				TRACE("[mp] New movie start pos: %d\r\n", p_movie_info->bookmarks.start);
				cMovieInfo.saveMovieInfo(*p_movie_info);	/* save immediately in xml file */
				cSelectedMenuBookStart[4].selected = false;	// clear for next bookmark menu
			} else if (!isWebTV && cSelectedMenuBookStart[5].selected == true) {
				/* Moviebrowser movie end bookmark */
				p_movie_info->bookmarks.end = play_sec;
				TRACE("[mp]  New movie end pos: %d\r\n", p_movie_info->bookmarks.start);
				cMovieInfo.saveMovieInfo(*p_movie_info);	/* save immediately in xml file */
				cSelectedMenuBookStart[5].selected = false;	// clear for next bookmark menu
			}
		}
	} else if (msg == NeutrinoMessages::SHOW_EPG && p_movie_info) {
		CTimeOSD::mode m_mode = FileTime.getMode();
		bool restore = FileTime.IsVisible();
		if (restore)
			FileTime.kill();
		InfoClock->enableInfoClock(false);

		cMovieInfo.showMovieInfo(*p_movie_info);

		InfoClock->enableInfoClock(true);
		if (restore) {
			FileTime.setMode(m_mode);
			FileTime.update(position, duration);
		}
	}
	return;
}

void CMoviePlayerGui::UpdatePosition()
{
	if(playback->GetPosition(position, duration)) {
		if(duration > 100)
			file_prozent = (unsigned char) (position / (duration / 100));
		FileTime.update(position, duration);
#ifdef DEBUG
		printf("CMoviePlayerGui::PlayFile: speed %d position %d duration %d (%d, %d%%)\n", speed, position, duration, duration-position, file_prozent);
#endif
	}
}

void CMoviePlayerGui::showHelpTS()
{
	Helpbox helpbox;
	helpbox.addLine(NEUTRINO_ICON_BUTTON_RED, g_Locale->getText(LOCALE_MOVIEPLAYER_TSHELP1));
	helpbox.addLine(NEUTRINO_ICON_BUTTON_GREEN, g_Locale->getText(LOCALE_MOVIEPLAYER_TSHELP2));
	helpbox.addLine(NEUTRINO_ICON_BUTTON_YELLOW, g_Locale->getText(LOCALE_MOVIEPLAYER_TSHELP3));
	helpbox.addLine(NEUTRINO_ICON_BUTTON_BLUE, g_Locale->getText(LOCALE_MOVIEPLAYER_TSHELP4));
	helpbox.addLine(NEUTRINO_ICON_BUTTON_MENU, g_Locale->getText(LOCALE_MOVIEPLAYER_TSHELP5));
	helpbox.addLine(NEUTRINO_ICON_BUTTON_1, g_Locale->getText(LOCALE_MOVIEPLAYER_TSHELP6));
	helpbox.addLine(NEUTRINO_ICON_BUTTON_3, g_Locale->getText(LOCALE_MOVIEPLAYER_TSHELP7));
	helpbox.addLine(NEUTRINO_ICON_BUTTON_4, g_Locale->getText(LOCALE_MOVIEPLAYER_TSHELP8));
	helpbox.addLine(NEUTRINO_ICON_BUTTON_6, g_Locale->getText(LOCALE_MOVIEPLAYER_TSHELP9));
	helpbox.addLine(NEUTRINO_ICON_BUTTON_7, g_Locale->getText(LOCALE_MOVIEPLAYER_TSHELP10));
	helpbox.addLine(NEUTRINO_ICON_BUTTON_9, g_Locale->getText(LOCALE_MOVIEPLAYER_TSHELP11));
	helpbox.addLine(g_Locale->getText(LOCALE_MOVIEPLAYER_TSHELP12));
	helpbox.show(LOCALE_MESSAGEBOX_INFO);
}

void CMoviePlayerGui::selectChapter()
{
	if (!is_file_player)
		return;

	std::vector<int> positions; std::vector<std::string> titles;
	playback->GetChapters(positions, titles);
	std::vector<int> playlists; std::vector<std::string> ptitles;
	int current;
	playback->GetTitles(playlists, ptitles, current);

	if (positions.empty() && playlists.empty())
		return;

	CMenuWidget ChSelector(LOCALE_MOVIEBROWSER_MENU_MAIN_BOOKMARKS, NEUTRINO_ICON_AUDIO);
	ChSelector.addItem(GenericMenuCancel);

	int pselect = -1;
	CMenuSelectorTarget * pselector = new CMenuSelectorTarget(&pselect);

	int select = -1;
	CMenuSelectorTarget * selector = new CMenuSelectorTarget(&select);
	char cnt[5];
	if (!playlists.empty()) {
		ChSelector.addItem(new CMenuSeparator(CMenuSeparator::LINE | CMenuSeparator::STRING, LOCALE_MOVIEPLAYER_TITLES));
		for (unsigned i = 0; i < playlists.size(); i++) {
			sprintf(cnt, "%d", i);
			CMenuForwarder * item = new CMenuForwarder(ptitles[i].c_str(), current != playlists[i], NULL, pselector, cnt);
			ChSelector.addItem(item);
		}
	}

	if (!positions.empty()) {
		ChSelector.addItem(new CMenuSeparator(CMenuSeparator::LINE | CMenuSeparator::STRING, LOCALE_MOVIEPLAYER_CHAPTERS));
	for (unsigned i = 0; i < positions.size(); i++) {
		sprintf(cnt, "%d", i);
		CMenuForwarder * item = new CMenuForwarder(titles[i].c_str(), true, NULL, selector, cnt, CRCInput::convertDigitToKey(i + 1));
		ChSelector.addItem(item, position > positions[i]);
	}
	}
	ChSelector.exec(NULL, "");
	delete selector;
	delete pselector;
	printf("CMoviePlayerGui::selectChapter: selected %d (%d)\n", select, (select >= 0) ? positions[select] : -1);
	printf("CMoviePlayerGui::selectChapter: pselected %d (%d)\n", pselect, (pselect >= 0) ? playlists[pselect] : -1);
	if(select >= 0) {
		playback->SetPosition(positions[select], true);
	} else if (pselect >= 0) {
		numsubs = numpida = 0;
		currentspid = -1;
		currentapid = 0;
		playback->SetTitle(playlists[pselect]);
	}
}

void CMoviePlayerGui::selectSubtitle()
{
	if (!is_file_player)
		return;

	CMenuWidget APIDSelector(LOCALE_SUBTITLES_HEAD, NEUTRINO_ICON_AUDIO);
	APIDSelector.addIntroItems();

	int select = -1;
	CMenuSelectorTarget * selector = new CMenuSelectorTarget(&select);
	if(!numsubs)
		playback->FindAllSubs(spids, sub_supported, &numsubs, slanguage);
	CMenuOptionStringChooser * sc = new CMenuOptionStringChooser(LOCALE_SUBTITLES_CHARSET, &g_settings.subs_charset, true, NULL, CRCInput::RC_red, NEUTRINO_ICON_BUTTON_RED, true);
	sc->addOption("UTF-8");
	sc->addOption("UCS-2");
	sc->addOption("CP1250");
	sc->addOption("CP1251");
	sc->addOption("CP1252");
	sc->addOption("CP1253");
	sc->addOption("KOI8-R");

	APIDSelector.addItem(sc);
	APIDSelector.addItem(GenericMenuSeparatorLine);
	char cnt[5];
	unsigned int count;
	for (count = 0; count < numsubs; count++) {
		bool enabled = sub_supported[count] && (currentspid != spids[count]);
		std::string title = slanguage[count];
		if (title.empty()) {
			char pidnumber[20];
			sprintf(pidnumber, "Stream %d %X", count + 1, spids[count]);
			title = pidnumber;
		}
		sprintf(cnt, "%d", count);
		CMenuForwarder * item = new CMenuForwarder(title.c_str(), enabled, NULL, selector, cnt, CRCInput::convertDigitToKey(count + 1));
		APIDSelector.addItem(item);
	}
	sprintf(cnt, "%d", count);
	APIDSelector.addItem(new CMenuForwarder(LOCALE_SUBTITLES_STOP, true, NULL, selector, cnt, CRCInput::RC_stop, NEUTRINO_ICON_BUTTON_STOP), currentspid > 0);

	APIDSelector.exec(NULL, "");
	delete selector;
	printf("CMoviePlayerGui::selectSubtitle: selected %d (%x) current %x\n", select, (select >= 0) ? spids[select] : -1, currentspid);
	if((select >= 0) && (select < numsubs) && (currentspid != spids[select])) {
		currentspid = spids[select];
		ext_subs = (currentspid == 0x1FFF);
		playback->SelectSubtitles(currentspid);
		printf("[movieplayer] spid changed to %d\n", currentspid);
	} else if ( select > 0) {
		ext_subs = false;
		currentspid = -1;
		playback->SelectSubtitles(currentspid);
		printf("[movieplayer] spid changed to %d\n", currentspid);
	}
}

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

void CMoviePlayerGui::clearSubtitle()
{
	if ((max_x-min_x > 0) && (max_y-min_y > 0))
		frameBuffer->paintBackgroundBoxRel(min_x, min_y, max_x-min_x, max_y-min_y);

	min_x = CFrameBuffer::getInstance()->getScreenWidth();
	min_y = CFrameBuffer::getInstance()->getScreenHeight();
	max_x = CFrameBuffer::getInstance()->getScreenX();
	max_y = CFrameBuffer::getInstance()->getScreenY();
	end_time = 0;
}

fb_pixel_t * simple_resize32(uint8_t * orgin, uint32_t * colors, int nb_colors, int ox, int oy, int dx, int dy);

bool CMoviePlayerGui::convertSubtitle(std::string &text)
{
	bool ret = false;
	iconv_t cd = iconv_open("UTF-8", g_settings.subs_charset.c_str());
	if (cd == (iconv_t)-1) {
		perror("iconv_open");
		return ret;
	}
	size_t ilen = text.length();
	size_t olen = ilen*4;
	size_t len = olen;
	char * buf = (char *) calloc(olen + 1, 1);
	if (buf == NULL) {
		iconv_close(cd);
		return ret;
	}
	char * out = buf;
	char * in = (char *) text.c_str();
	if (iconv(cd, &in, &ilen, &out, &olen) == (size_t)-1) {
		printf("CMoviePlayerGui::convertSubtitle: iconv error\n");
	}
	else {
		memset(buf + (len - olen), 0, olen);
		text = buf;
		ret = true;
	}

	free(buf);
	iconv_close(cd);
	return true;
}
void CMoviePlayerGui::showSubtitle(neutrino_msg_data_t data)
{
	if (!data) {
		if (end_time && time_monotonic_ms() > end_time) {
			printf("************************* hide subs *************************\n");
			clearSubtitle();
		}
		return;
	}
	AVSubtitle * sub = (AVSubtitle *) data;

	printf("************************* EVT_SUBT_MESSAGE: num_rects %d fmt %d *************************\n",  sub->num_rects, sub->format);
	if (!sub->num_rects)
		return;

	if (sub->format == 0) {
		int xres = 0, yres = 0, framerate;
		videoDecoder->getPictureInfo(xres, yres, framerate);

		double xc = (double) CFrameBuffer::getInstance()->getScreenWidth(/*true*/)/(double) xres;
		double yc = (double) CFrameBuffer::getInstance()->getScreenHeight(/*true*/)/(double) yres;

		clearSubtitle();

		for (unsigned i = 0; i < sub->num_rects; i++) {
			uint32_t * colors = (uint32_t *) sub->rects[i]->pict.data[1];

			int nw = (double) sub->rects[i]->w * xc;
			int nh = (double) sub->rects[i]->h * yc;
			int xoff = (double) sub->rects[i]->x * xc;
			int yoff = (double) sub->rects[i]->y * yc;

			printf("Draw: #%d at %d,%d size %dx%d colors %d (x=%d y=%d w=%d h=%d) \n", i+1,
					sub->rects[i]->x, sub->rects[i]->y, sub->rects[i]->w, sub->rects[i]->h,
					sub->rects[i]->nb_colors, xoff, yoff, nw, nh);

			fb_pixel_t * newdata = simple_resize32 (sub->rects[i]->pict.data[0], colors,
					sub->rects[i]->nb_colors, sub->rects[i]->w, sub->rects[i]->h, nw, nh);
			frameBuffer->blit2FB(newdata, nw, nh, xoff, yoff);
			free(newdata);

			min_x = std::min(min_x, xoff);
			max_x = std::max(max_x, xoff + nw);
			min_y = std::min(min_y, yoff);
			max_y = std::max(max_y, yoff + nh);
		}
		end_time = sub->end_display_time + time_monotonic_ms();
		avsubtitle_free(sub);
		delete sub;
		return;
	}
	std::vector<std::string> subtext;
	for (unsigned i = 0; i < sub->num_rects; i++) {
		char * txt = NULL;
		if (sub->rects[i]->type == SUBTITLE_TEXT)
			txt = sub->rects[i]->text;
		else if (sub->rects[i]->type == SUBTITLE_ASS)
			txt = sub->rects[i]->ass;
		printf("subt[%d] type %d [%s]\n", i, sub->rects[i]->type, txt ? txt : "");
		if (txt) {
			int len = strlen(txt);
			if (len > 10 && memcmp(txt, "Dialogue: ", 10) == 0) {
				char* p = txt;
				int skip_commas = 4;
				/* skip ass times */
				for (int j = 0; j < skip_commas && *p != '\0'; p++)
					if (*p == ',')
						j++;
				/* skip ass tags */
				if (*p == '{') {
					char * d = strchr(p, '}');
					if (d)
						p += d - p + 1;
				}
				char * d = strchr(p, '{');
				if (d && strchr(d, '}'))
					*d = 0;

				len = strlen(p);
				/* remove newline */
				for (int j = len-1; j > 0; j--) {
					if (p[j] == '\n' || p[j] == '\r')
						p[j] = 0;
					else
						break;
				}
				if (*p == '\0')
					continue;
				txt = p;
			}
			//printf("title: [%s]\n", txt);
			std::string str(txt);
			size_t start = 0, end = 0;
			/* split string with \N as newline */
			std::string delim("\\N");
			while ((end = str.find(delim, start)) != string::npos) {
				subtext.push_back(str.substr(start, end - start));
				start = end + 2;
			}
			subtext.push_back(str.substr(start));

		}
	}
	for (unsigned i = 0; i < subtext.size(); i++) {
		if (!isUTF8(subtext[i])) {
			if (g_settings.subs_charset != "UTF-8")
				convertSubtitle(subtext[i]);
			else
				subtext[i] = convertLatin1UTF8(subtext[i]);
		}
		printf("subtext %d: [%s]\n", i, subtext[i].c_str());
	}
	printf("********************************************************************\n");

	if (!subtext.empty()) {
		int sh = frameBuffer->getScreenHeight();
		int sw = frameBuffer->getScreenWidth();
		int h = g_Font[SNeutrinoSettings::FONT_TYPE_SUBTITLES]->getHeight();
		int height = h*subtext.size();

		clearSubtitle();

		int x[subtext.size()];
		int y[subtext.size()];
		for (unsigned i = 0; i < subtext.size(); i++) {
			int w = g_Font[SNeutrinoSettings::FONT_TYPE_SUBTITLES]->getRenderWidth (subtext[i]);
			x[i] = (sw - w) / 2;
			y[i] = sh - height + h*(i + 1);
			min_x = std::min(min_x, x[i]);
			max_x = std::max(max_x, x[i]+w);
			min_y = std::min(min_y, y[i]-h);
			max_y = std::max(max_y, y[i]);
		}

		frameBuffer->paintBoxRel(min_x, min_y, max_x - min_x, max_y-min_y, COL_MENUCONTENT_PLUS_0);

		for (unsigned i = 0; i < subtext.size(); i++)
			g_Font[SNeutrinoSettings::FONT_TYPE_SUBTITLES]->RenderString(x[i], y[i], sw, subtext[i].c_str(), COL_MENUCONTENT_TEXT);

		end_time = sub->end_display_time + time_monotonic_ms();
	}
	avsubtitle_free(sub);
	delete sub;
}

void CMoviePlayerGui::selectAutoLang()
{
	if (ext_subs) {
		playback->FindAllSubs(spids, sub_supported, &numsubs, slanguage);
		for (unsigned count = 0; count < numsubs; count++) {
			if (spids[count] == 0x1FFF) {
				currentspid = spids[count];
				playback->SelectSubtitles(currentspid);
			}
		}
	}
	if(g_settings.auto_lang &&  (numpida > 1)) {
		int pref_idx = -1;

		playback->FindAllPids(apids, ac3flags, &numpida, language);
		for(int i = 0; i < 3; i++) {
			for (unsigned j = 0; j < numpida; j++) {
				std::map<std::string, std::string>::const_iterator it;
				for(it = iso639.begin(); it != iso639.end(); ++it) {
					if (g_settings.pref_lang[i] == it->second && strncasecmp(language[j].c_str(), it->first.c_str(), 3) == 0) {
						bool enabled = true;
						// TODO: better check of supported
						std::string audioname;
						addAudioFormat(j, audioname, enabled);
						if (enabled)
							pref_idx = j;
						break;
					}
				}
				if (pref_idx >= 0)
					break;
			}
			if (pref_idx >= 0)
				break;
		}
		if (pref_idx >= 0) {
			currentapid = apids[pref_idx];
			currentac3 = ac3flags[pref_idx];
			playback->SetAPid(currentapid, currentac3);
		}
	}
}

void CMoviePlayerGui::parsePlaylist(CFile *file)
{
	std::ifstream infile;
	char cLine[1024];
	char name[1024] = { 0 };
	infile.open(file->Name.c_str(), std::ifstream::in);
	while (infile.good())
	{
		infile.getline(cLine, sizeof(cLine));
		if (cLine[strlen(cLine)-1]=='\r')
			cLine[strlen(cLine)-1]=0;

		int dur;
		sscanf(cLine, "#EXTINF:%d,%[^\n]\n", &dur, name);
		if (strlen(cLine) > 0 && cLine[0]!='#')
		{
			char *url = NULL;
			if ( (url = strstr(cLine, "http://")) || (url = strstr(cLine, "rtmp://")) || (url = strstr(cLine, "rtsp://")) ){
				if (url != NULL) {
					printf("name %s [%d] url: %s\n", name, dur, url);
					full_name = url;
					if(strlen(name))
						file_name = name;
				}
			}
		}
	}
}

bool CMoviePlayerGui::mountIso(CFile *file)
{
	printf("ISO file passed: %s\n", file->Name.c_str());
	safe_mkdir((char *)ISO_MOUNT_POINT);
	if (my_system(5, "mount", "-o", "loop", file->Name.c_str(), ISO_MOUNT_POINT) == 0) {
		makeFilename();
		full_name = "/media/iso";
		iso_file = true;
		return true;
	}
	return false;
}

void CMoviePlayerGui::makeScreenShot(bool autoshot, bool forcover)
{
	if (autoshot) {
		if (autoshot_done || !g_settings.auto_cover)
			return;
		autoshot_done = true;
	}

	bool cover = autoshot || g_settings.screenshot_cover || forcover;
	char ending[(sizeof(int)*2) + 6] = ".jpg";
	if (!cover)
		snprintf(ending, sizeof(ending) - 1, "_%x.jpg", position);

	std::string fname = full_name;
	std::string::size_type pos = fname.find_last_of('.');
	if (pos != std::string::npos) {
		fname.replace(pos, fname.length(), ending);
	} else
		fname += ending;

	if (autoshot && !access(fname.c_str(), F_OK)) {
		printf("CMoviePlayerGui::makeScreenShot: cover [%s] already exist..\n", fname.c_str());
		return;
	}

	if (!cover) {
		pos = fname.find_last_of('/');
		if(pos != std::string::npos)
			fname.replace(0, pos, g_settings.screenshot_dir);
	}


	CScreenShot * sc = new CScreenShot(fname);
	if (cover) {
		sc->EnableVideo(true);
	}
	if (autoshot || forcover) {
		int xres = 0, yres = 0, framerate;
		videoDecoder->getPictureInfo(xres, yres, framerate);
		if (xres && yres) {
			int w = std::min(300, xres);
			int h = (float) yres / ((float) xres / (float) w);
			sc->SetSize(w, h);
		}
	}
	sc->Start();
}
