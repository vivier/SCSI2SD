//	Copyright (C) 2014 Michael McMaster <michael@codesrc.com>
//
//	This file is part of SCSI2SD.
//
//	SCSI2SD is free software: you can redistribute it and/or modify
//	it under the terms of the GNU General Public License as published by
//	the Free Software Foundation, either version 3 of the License, or
//	(at your option) any later version.
//
//	SCSI2SD is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//	GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License
//	along with SCSI2SD.  If not, see <http://www.gnu.org/licenses/>.


// For compilers that support precompilation, includes "wx/wx.h".
#include <wx/wxprec.h>
#ifndef WX_PRECOMP
#include <wx/wx.h>
#endif

#include <wx/app.h>
#include <wx/filedlg.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/notebook.h>
#include <wx/progdlg.h>
#include <wx/utils.h>
#include <wx/wfstream.h>
#include <wx/windowptr.h>
#include <wx/thread.h>
#include <wx/txtstrm.h>

#include <zipper.hh>

#include "ConfigUtil.hh"
#include "TargetPanel.hh"
#include "SCSI2SD_Bootloader.hh"
#include "SCSI2SD_HID.hh"
#include "Firmware.hh"

#include <algorithm>
#include <iomanip>
#include <vector>
#include <set>
#include <sstream>

#if __cplusplus >= 201103L
#include <cstdint>
#include <memory>
using std::shared_ptr;
#else
#include <stdint.h>
#include <tr1/memory>
using std::tr1::shared_ptr;
#endif

#define MIN_FIRMWARE_VERSION 0x0400

/*
 *      SCSI opcodes
 */

#define TEST_UNIT_READY       0x00
#define REZERO_UNIT           0x01
#define REQUEST_SENSE         0x03
#define FORMAT_UNIT           0x04
#define READ_BLOCK_LIMITS     0x05
#define REASSIGN_BLOCKS       0x07
#define READ_6                0x08
#define WRITE_6               0x0a
#define SEEK_6                0x0b
#define READ_REVERSE          0x0f
#define WRITE_FILEMARKS       0x10
#define SPACE                 0x11
#define INQUIRY               0x12
#define RECOVER_BUFFERED_DATA 0x14
#define MODE_SELECT           0x15
#define RESERVE               0x16
#define RELEASE               0x17
#define COPY                  0x18
#define ERASE                 0x19
#define MODE_SENSE            0x1a
#define START_STOP            0x1b
#define RECEIVE_DIAGNOSTIC    0x1c
#define SEND_DIAGNOSTIC       0x1d
#define ALLOW_MEDIUM_REMOVAL  0x1e

#define SET_WINDOW            0x24
#define READ_CAPACITY         0x25
#define READ_10               0x28
#define WRITE_10              0x2a
#define SEEK_10               0x2b
#define WRITE_VERIFY          0x2e
#define VERIFY                0x2f
#define SEARCH_HIGH           0x30
#define SEARCH_EQUAL          0x31
#define SEARCH_LOW            0x32
#define SET_LIMITS            0x33
#define PRE_FETCH             0x34
#define READ_POSITION         0x34
#define SYNCHRONIZE_CACHE     0x35
#define LOCK_UNLOCK_CACHE     0x36
#define READ_DEFECT_DATA      0x37
#define MEDIUM_SCAN           0x38
#define COMPARE               0x39
#define COPY_VERIFY           0x3a
#define WRITE_BUFFER          0x3b
#define READ_BUFFER           0x3c
#define UPDATE_BLOCK          0x3d
#define READ_LONG             0x3e
#define WRITE_LONG            0x3f
#define CHANGE_DEFINITION     0x40
#define WRITE_SAME            0x41
#define READ_TOC              0x43
#define REPORT_DENSITY        0x44
#define LOG_SELECT            0x4c
#define LOG_SENSE             0x4d
#define MODE_SELECT_10        0x55
#define RESERVE_10            0x56
#define RELEASE_10            0x57
#define MODE_SENSE_10         0x5a
#define PERSISTENT_RESERVE_IN 0x5e
#define PERSISTENT_RESERVE_OUT 0x5f
#define MOVE_MEDIUM           0xa5
#define READ_12               0xa8
#define WRITE_12              0xaa
#define WRITE_VERIFY_12       0xae
#define SEARCH_HIGH_12        0xb0
#define SEARCH_EQUAL_12       0xb1
#define SEARCH_LOW_12         0xb2
#define READ_ELEMENT_STATUS   0xb8
#define SEND_VOLUME_TAG       0xb6
#define WRITE_LONG_2          0xea

using namespace SCSI2SD;

class ProgressWrapper
{
public:
	void setProgressDialog(
		const wxWindowPtr<wxGenericProgressDialog>& dlg,
		size_t maxRows)
	{
		myProgressDialog = dlg;
		myMaxRows = maxRows;
		myNumRows = 0;
	}

	void clearProgressDialog()
	{
		myProgressDialog->Show(false);
		myProgressDialog.reset();
	}

	void update(unsigned char arrayId, unsigned short rowNum)
	{
		if (!myProgressDialog) return;

		myNumRows++;

		std::stringstream ss;
		ss << "Writing flash array " <<
			static_cast<int>(arrayId) << " row " <<
			static_cast<int>(rowNum);
		wxLogMessage("%s", ss.str());
		myProgressDialog->Update(myNumRows, ss.str());
	}

private:
	wxWindowPtr<wxGenericProgressDialog> myProgressDialog;
	size_t myMaxRows;
	size_t myNumRows;
};
static ProgressWrapper TheProgressWrapper;

extern "C"
void ProgressUpdate(unsigned char arrayId, unsigned short rowNum)
{
	TheProgressWrapper.update(arrayId, rowNum);
}

namespace
{

static uint8_t sdCrc7(uint8_t* chr, uint8_t cnt, uint8_t crc)
{
	uint8_t a;
	for(a = 0; a < cnt; a++)
	{
		uint8_t data = chr[a];
		uint8_t i;
		for(i = 0; i < 8; i++)
		{
			crc <<= 1;
			if ((data & 0x80) ^ (crc & 0x80))
			{
				crc ^= 0x09;
			}
			data <<= 1;
		}
	}
	return crc & 0x7F;
}

class TimerLock
{
public:
	TimerLock(wxTimer* timer) :
		myTimer(timer),
		myInterval(myTimer->GetInterval())
	{
		myTimer->Stop();
	};

	virtual ~TimerLock()
	{
		if (myTimer && myInterval > 0)
		{
			myTimer->Start(myInterval);
		}
	}
private:
	wxTimer* myTimer;
	int myInterval;
};

class AppFrame : public wxFrame
{
public:
	AppFrame() :
		wxFrame(NULL, wxID_ANY, "scsi2sd-util", wxPoint(50, 50), wxSize(600, 700)),
		myInitialConfig(false),
		myTickCounter(0),
		myLastPollTime(0)
	{
		wxMenu *menuFile = new wxMenu();
		menuFile->Append(
			ID_SaveFile,
			"&Save to file...",
			"Save settings to local file.");
		menuFile->Append(
			ID_OpenFile,
			"&Open file...",
			"Load settings from local file.");
		menuFile->AppendSeparator();
		menuFile->Append(
			ID_ConfigDefaults,
			"Load &Defaults",
			"Load default configuration options.");
		menuFile->Append(
			ID_Firmware,
			"&Upgrade Firmware...",
			"Upgrade or inspect device firmware version.");
		menuFile->AppendSeparator();
		menuFile->Append(wxID_EXIT);

		wxMenu *menuWindow= new wxMenu();
		menuWindow->Append(
			ID_LogWindow,
			"Show &Log",
			"Show debug log window");

		wxMenu *menuDebug = new wxMenu();
		mySCSILogChk = menuDebug->AppendCheckItem(
			ID_SCSILog,
			"Log SCSI data",
			"Log SCSI commands");

		mySelfTestChk = menuDebug->AppendCheckItem(
			ID_SelfTest,
			"SCSI Standalone Self-Test",
			"SCSI Standalone Self-Test");

		wxMenu *menuHelp = new wxMenu();
		menuHelp->Append(wxID_ABOUT);

		wxMenuBar *menuBar = new wxMenuBar();
		menuBar->Append( menuFile, "&File" );
		menuBar->Append( menuDebug, "&Debug" );
		menuBar->Append( menuWindow, "&Window" );
		menuBar->Append( menuHelp, "&Help" );
		SetMenuBar( menuBar );

		CreateStatusBar();

		{
			wxPanel* cfgPanel = new wxPanel(this);
			wxFlexGridSizer *fgs = new wxFlexGridSizer(3, 1, 15, 15);
			cfgPanel->SetSizer(fgs);

			// Empty space below menu bar.
			fgs->Add(5, 5, wxALL);

			wxNotebook* tabs = new wxNotebook(cfgPanel, ID_Notebook);

			for (int i = 0; i < MAX_SCSI_TARGETS; ++i)
			{
				TargetPanel* target =
					new TargetPanel(tabs, ConfigUtil::Default(i));
				myTargets.push_back(target);
				std::stringstream ss;
				ss << "Device " << (i + 1);
				tabs->AddPage(target, ss.str());
				target->Fit();
			}
			tabs->Fit();
			fgs->Add(tabs);


			wxPanel* btnPanel = new wxPanel(cfgPanel);
			wxFlexGridSizer *btnFgs = new wxFlexGridSizer(1, 2, 5, 5);
			btnPanel->SetSizer(btnFgs);
			myLoadButton =
				new wxButton(btnPanel, ID_BtnLoad, wxT("Load from device"));
			btnFgs->Add(myLoadButton);
			mySaveButton =
				new wxButton(btnPanel, ID_BtnSave, wxT("Save to device"));
			btnFgs->Add(mySaveButton);
			fgs->Add(btnPanel);

			btnPanel->Fit();
			cfgPanel->Fit();
		}
		//Fit(); // Needed to reduce window size on Windows
		FitInside(); // Needed on Linux to prevent status bar overlap

		myLogWindow = new wxLogWindow(this, wxT("scsi2sd-util debug log"), true);
		myLogWindow->PassMessages(false); // Prevent messagebox popups

		myTimer = new wxTimer(this, ID_Timer);
		myTimer->Start(16); //ms, suitable for scsi debug logging
	}

private:
	wxLogWindow* myLogWindow;
	std::vector<TargetPanel*> myTargets;
	wxButton* myLoadButton;
	wxButton* mySaveButton;
	wxMenuItem* mySCSILogChk;
	wxMenuItem* mySelfTestChk;
	wxTimer* myTimer;
	shared_ptr<HID> myHID;
	shared_ptr<Bootloader> myBootloader;
	bool myInitialConfig;

	uint8_t myTickCounter;

	time_t myLastPollTime;

	void mmLogStatus(const std::string& msg)
	{
		// We set PassMessages to false on our log window to prevent popups, but
		// this also prevents wxLogStatus from updating the status bar.
		SetStatusText(msg);
		wxLogMessage(this, "%s", msg.c_str());
	}

	void onConfigChanged(wxCommandEvent& event)
	{
		evaluate();
	}

	void evaluate()
	{
		bool valid = true;

		// Check for duplicate SCSI IDs
		std::set<uint8_t> enabledID;

		// Check for overlapping SD sectors.
		std::vector<std::pair<uint32_t, uint64_t> > sdSectors;

		bool isTargetEnabled = false; // Need at least one enabled
		uint32_t autoStartSector = 0;
		for (size_t i = 0; i < myTargets.size(); ++i)
		{
			myTargets[i]->setAutoStartSector(autoStartSector);
			valid = myTargets[i]->evaluate() && valid;

			if (myTargets[i]->isEnabled())
			{
				isTargetEnabled = true;
				uint8_t scsiID = myTargets[i]->getSCSIId();
				if (enabledID.find(scsiID) != enabledID.end())
				{
					myTargets[i]->setDuplicateID(true);
					valid = false;
				}
				else
				{
					enabledID.insert(scsiID);
					myTargets[i]->setDuplicateID(false);
				}

				auto sdSectorRange = myTargets[i]->getSDSectorRange();
				for (auto it(sdSectors.begin()); it != sdSectors.end(); ++it)
				{
					if (sdSectorRange.first < it->second &&
						sdSectorRange.second > it->first)
					{
						valid = false;
						myTargets[i]->setSDSectorOverlap(true);
					}
					else
					{
						myTargets[i]->setSDSectorOverlap(false);
					}
				}
				sdSectors.push_back(sdSectorRange);
				autoStartSector = sdSectorRange.second;
			}
			else
			{
				myTargets[i]->setDuplicateID(false);
				myTargets[i]->setSDSectorOverlap(false);
			}
		}

		valid = valid && isTargetEnabled; // Need at least one.

		mySaveButton->Enable(
			valid &&
			myHID &&
			(myHID->getFirmwareVersion() >= MIN_FIRMWARE_VERSION));

		myLoadButton->Enable(
			myHID &&
			(myHID->getFirmwareVersion() >= MIN_FIRMWARE_VERSION));
	}


	enum
	{
		ID_ConfigDefaults = wxID_HIGHEST + 1,
		ID_Firmware,
		ID_Timer,
		ID_Notebook,
		ID_BtnLoad,
		ID_BtnSave,
		ID_LogWindow,
		ID_SCSILog,
		ID_SelfTest,
		ID_SaveFile,
		ID_OpenFile
	};

	void OnID_ConfigDefaults(wxCommandEvent& event)
	{
		for (size_t i = 0; i < myTargets.size(); ++i)
		{
			myTargets[i]->setConfig(ConfigUtil::Default(i));
		}
	}

	void OnID_SaveFile(wxCommandEvent& event)
	{
		TimerLock lock(myTimer);



		wxFileDialog dlg(
			this,
			"Save config settings",
			"",
			"",
			"XML files (*.xml)|*.xml",
			wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
		if (dlg.ShowModal() == wxID_CANCEL) return;

		wxFileOutputStream file(dlg.GetPath());
		if (!file.IsOk())
		{
			wxLogError("Cannot save settings to file '%s'.", dlg.GetPath());
			return;
		}

		wxTextOutputStream s(file);

		s << "<SCSI2SD>\n";

		for (size_t i = 0; i < myTargets.size(); ++i)
		{
			s << ConfigUtil::toXML(myTargets[i]->getConfig());
		}

		s << "</SCSI2SD>\n";
	}

	void OnID_OpenFile(wxCommandEvent& event)
	{
		TimerLock lock(myTimer);

		wxFileDialog dlg(
			this,
			"Load config settings",
			"",
			"",
			"XML files (*.xml)|*.xml",
			wxFD_OPEN | wxFD_FILE_MUST_EXIST);
		if (dlg.ShowModal() == wxID_CANCEL) return;

		try
		{
			std::vector<TargetConfig> configs(
				ConfigUtil::fromXML(std::string(dlg.GetPath())));

			size_t i;
			for (i = 0; i < configs.size() && i < myTargets.size(); ++i)
			{
				myTargets[i]->setConfig(configs[i]);
			}

			for (; i < myTargets.size(); ++i)
			{
				myTargets[i]->setConfig(ConfigUtil::Default(i));
			}
		}
		catch (std::exception& e)
		{
			wxLogError(
				"Cannot load settings from file '%s'.\n%s",
				dlg.GetPath(),
				e.what());

			wxMessageBox(
				e.what(),
				"Load error",
				wxOK | wxICON_ERROR);
		}
	}

	void OnID_Firmware(wxCommandEvent& event)
	{
		TimerLock lock(myTimer);
		doFirmwareUpdate();
	}

	void OnID_LogWindow(wxCommandEvent& event)
	{
		myLogWindow->Show();
	}

	void doFirmwareUpdate()
	{
		wxFileDialog dlg(
			this,
			"Load firmware file",
			"",
			"",
			"SCSI2SD Firmware files (*.scsi2sd;*.cyacd)|*.cyacd;*.scsi2sd",
			wxFD_OPEN | wxFD_FILE_MUST_EXIST);
		if (dlg.ShowModal() == wxID_CANCEL) return;

		std::string filename(dlg.GetPath());

		wxWindowPtr<wxGenericProgressDialog> progress(
			new wxGenericProgressDialog(
				"Searching for bootloader",
				"Searching for bootloader",
				100,
				this,
				wxPD_AUTO_HIDE | wxPD_CAN_ABORT)
				);
		mmLogStatus("Searching for bootloader");
		while (true)
		{
			try
			{
				if (!myHID) myHID.reset(HID::Open());
				if (myHID)
				{
					mmLogStatus("Resetting SCSI2SD into bootloader");

					myHID->enterBootloader();
					myHID.reset();
				}


				if (!myBootloader)
				{
					myBootloader.reset(Bootloader::Open());
					if (myBootloader)
					{
						mmLogStatus("Bootloader found");
						break;
					}
				}

				else if (myBootloader)
				{
					// Verify the USB HID connection is valid
					if (!myBootloader->ping())
					{
						mmLogStatus("Bootloader ping failed");
						myBootloader.reset();
					}
					else
					{
						mmLogStatus("Bootloader found");
						break;
					}
				}
			}
			catch (std::exception& e)
			{
				mmLogStatus(e.what());
				myHID.reset();
				myBootloader.reset();
			}
			wxMilliSleep(100);
			if (!progress->Pulse())
			{
				return; // user cancelled.
			}
		}

		int totalFlashRows = 0;
		std::string tmpFile;
		try
		{
			zipper::ReaderPtr reader(new zipper::FileReader(filename));
			zipper::Decompressor decomp(reader);
			std::vector<zipper::CompressedFilePtr> files(decomp.getEntries());
			for (auto it(files.begin()); it != files.end(); it++)
			{
				if (myBootloader->isCorrectFirmware((*it)->getPath()))
				{
					std::stringstream msg;
					msg << "Found firmware entry " << (*it)->getPath() <<
						" within archive " << filename;
					mmLogStatus(msg.str());
					tmpFile =
						wxFileName::CreateTempFileName(
							wxT("SCSI2SD_Firmware"), static_cast<wxFile*>(NULL)
							);
					zipper::FileWriter out(tmpFile);
					(*it)->decompress(out);
					msg.clear();
					msg << "Firmware extracted to " << tmpFile;
					mmLogStatus(msg.str());
					break;
				}
			}

			if (tmpFile.empty())
			{
				// TODO allow "force" option
				wxMessageBox(
					"Wrong filename",
					"Wrong filename",
					wxOK | wxICON_ERROR);
				return;
			}

			Firmware firmware(tmpFile);
			totalFlashRows = firmware.totalFlashRows();
		}
		catch (std::exception& e)
		{
			mmLogStatus(e.what());
			std::stringstream msg;
			msg << "Could not open firmware file: " << e.what();
			wxMessageBox(
				msg.str(),
				"Bad file",
				wxOK | wxICON_ERROR);
			wxRemoveFile(tmpFile);
			return;
		}

		{
			wxWindowPtr<wxGenericProgressDialog> progress(
				new wxGenericProgressDialog(
					"Loading firmware",
					"Loading firmware",
					totalFlashRows,
					this,
					wxPD_AUTO_HIDE | wxPD_REMAINING_TIME)
					);
			TheProgressWrapper.setProgressDialog(progress, totalFlashRows);
		}

		std::stringstream msg;
		msg << "Upgrading firmware from file: " << tmpFile;
		mmLogStatus(msg.str());

		try
		{
			myBootloader->load(tmpFile, &ProgressUpdate);
			TheProgressWrapper.clearProgressDialog();

			wxMessageBox(
				"Firmware update successful",
				"Firmware OK",
				wxOK);
			mmLogStatus("Firmware update successful");


			myHID.reset();
			myBootloader.reset();
		}
		catch (std::exception& e)
		{
			TheProgressWrapper.clearProgressDialog();
			mmLogStatus(e.what());
			myHID.reset();
			myBootloader.reset();

			wxMessageBox(
				"Firmware Update Failed",
				e.what(),
				wxOK | wxICON_ERROR);

			wxRemoveFile(tmpFile);
		}
	}

	void dumpSCSICommand(std::vector<uint8_t> buf)
        {
		std::stringstream msg;
		msg << std::hex;
		static std::vector<uint8_t> last(64);
		int ticks;
		typedef enum
		{
			GOOD = 0,
			CHECK_CONDITION = 2,
			BUSY = 0x8,
			INTERMEDIATE = 0x10,
			CONFLICT = 0x18
		} SCSI_STATUS;
		typedef enum
		{
			NO_SENSE = 0,
			RECOVERED_ERROR = 1,
			NOT_READY = 2,
			MEDIUM_ERROR = 3,
			HARDWARE_ERROR = 4,
			ILLEGAL_REQUEST = 5,
			UNIT_ATTENTION = 6,
			DATA_PROTECT = 7,
			BLANK_CHECK = 8,
			VENDOR_SPECIFIC = 9,
			COPY_ABORTED = 0xA,
			ABORTED_COMMAND = 0xB,
			EQUAL = 0xC,
			VOLUME_OVERFLOW = 0xD,
			MISCOMPARE = 0xE,
			RESERVED = 0xF
		} SCSI_SENSE;
		typedef enum
		{
			ADDRESS_MARK_NOT_FOUND_FOR_DATA_FIELD                  = 0x1300,
			ADDRESS_MARK_NOT_FOUND_FOR_ID_FIELD                    = 0x1200,
			CANNOT_READ_MEDIUM_INCOMPATIBLE_FORMAT                 = 0x3002,
			CANNOT_READ_MEDIUM_UNKNOWN_FORMAT                      = 0x3001,
			CHANGED_OPERATING_DEFINITION                           = 0x3F02,
			COMMAND_PHASE_ERROR                                    = 0x4A00,
			COMMAND_SEQUENCE_ERROR                                 = 0x2C00,
			COMMANDS_CLEARED_BY_ANOTHER_INITIATOR                  = 0x2F00,
			COPY_CANNOT_EXECUTE_SINCE_HOST_CANNOT_DISCONNECT       = 0x2B00,
			DATA_PATH_FAILURE                                      = 0x4100,
			DATA_PHASE_ERROR                                       = 0x4B00,
			DATA_SYNCHRONIZATION_MARK_ERROR                        = 0x1600,
			DEFECT_LIST_ERROR                                      = 0x1900,
			DEFECT_LIST_ERROR_IN_GROWN_LIST                        = 0x1903,
			DEFECT_LIST_ERROR_IN_PRIMARY_LIST                      = 0x1902,
			DEFECT_LIST_NOT_AVAILABLE                              = 0x1901,
			DEFECT_LIST_NOT_FOUND                                  = 0x1C00,
			DEFECT_LIST_UPDATE_FAILURE                             = 0x3201,
			ERROR_LOG_OVERFLOW                                     = 0x0A00,
			ERROR_TOO_LONG_TO_CORRECT                              = 0x1102,
			FORMAT_COMMAND_FAILED                                  = 0x3101,
			GROWN_DEFECT_LIST_NOT_FOUND                            = 0x1C02,
			IO_PROCESS_TERMINATED                                  = 0x0006,
			ID_CRC_OR_ECC_ERROR                                    = 0x1000,
			ILLEGAL_FUNCTION                                       = 0x2200,
			INCOMPATIBLE_MEDIUM_INSTALLED                          = 0x3000,
			INITIATOR_DETECTED_ERROR_MESSAGE_RECEIVED              = 0x4800,
			INQUIRY_DATA_HAS_CHANGED                               = 0x3F03,
			INTERNAL_TARGET_FAILURE                                = 0x4400,
			INVALID_BITS_IN_IDENTIFY_MESSAGE                       = 0x3D00,
			INVALID_COMMAND_OPERATION_CODE                         = 0x2000,
			INVALID_FIELD_IN_CDB                                   = 0x2400,
			INVALID_FIELD_IN_PARAMETER_LIST                        = 0x2600,
			INVALID_MESSAGE_ERROR                                  = 0x4900,
			LOG_COUNTER_AT_MAXIMUM                                 = 0x5B02,
			LOG_EXCEPTION                                          = 0x5B00,
			LOG_LIST_CODES_EXHAUSTED                               = 0x5B03,
			LOG_PARAMETERS_CHANGED                                 = 0x2A02,
			LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE                     = 0x2100,
			LOGICAL_UNIT_COMMUNICATION_FAILURE                     = 0x0800,
			LOGICAL_UNIT_COMMUNICATION_PARITY_ERROR                = 0x0802,
			LOGICAL_UNIT_COMMUNICATION_TIMEOUT                     = 0x0801,
			LOGICAL_UNIT_DOES_NOT_RESPOND_TO_SELECTION             = 0x0500,
			LOGICAL_UNIT_FAILED_SELF_CONFIGURATION                 = 0x4C00,
			LOGICAL_UNIT_HAS_NOT_SELF_CONFIGURED_YET               = 0x3E00,
			LOGICAL_UNIT_IS_IN_PROCESS_OF_BECOMING_READY           = 0x0401,
			LOGICAL_UNIT_NOT_READY_CAUSE_NOT_REPORTABLE            = 0x0400,
			LOGICAL_UNIT_NOT_READY_FORMAT_IN_PROGRESS              = 0x0404,
			LOGICAL_UNIT_NOT_READY_INITIALIZING_COMMAND_REQUIRED   = 0x0402,
			LOGICAL_UNIT_NOT_READY_MANUAL_INTERVENTION_REQUIRED    = 0x0403,
			LOGICAL_UNIT_NOT_SUPPORTED                             = 0x2500,
			MECHANICAL_POSITIONING_ERROR                           = 0x1501,
			MEDIA_LOAD_OR_EJECT_FAILED                             = 0x5300,
			MEDIUM_FORMAT_CORRUPTED                                = 0x3100,
			MEDIUM_NOT_PRESENT                                     = 0x3A00,
			MEDIUM_REMOVAL_PREVENTED                               = 0x5302,
			MESSAGE_ERROR                                          = 0x4300,
			MICROCODE_HAS_BEEN_CHANGED                             = 0x3F01,
			MISCOMPARE_DURING_VERIFY_OPERATION                     = 0x1D00,
			MISCORRECTED_ERROR                                     = 0x110A,
			MODE_PARAMETERS_CHANGED                                = 0x2A01,
			MULTIPLE_PERIPHERAL_DEVICES_SELECTED                   = 0x0700,
			MULTIPLE_READ_ERRORS                                   = 0x1103,
			NO_ADDITIONAL_SENSE_INFORMATION                        = 0x0000,
			NO_DEFECT_SPARE_LOCATION_AVAILABLE                     = 0x3200,
			NO_INDEX_SECTOR_SIGNAL                                 = 0x0100,
			NO_REFERENCE_POSITION_FOUND                            = 0x0600,
			NO_SEEK_COMPLETE                                       = 0x0200,
			NOT_READY_TO_READY_TRANSITION_MEDIUM_MAY_HAVE_CHANGED  = 0x2800,
			OPERATOR_MEDIUM_REMOVAL_REQUEST                        = 0x5A01,
			OPERATOR_REQUEST_OR_STATE_CHANGE_INPUT                 = 0x5A00,
			OPERATOR_SELECTED_WRITE_PERMIT                         = 0x5A03,
			OPERATOR_SELECTED_WRITE_PROTECT                        = 0x5A02,
			OVERLAPPED_COMMANDS_ATTEMPTED                          = 0x4E00,
			PARAMETER_LIST_LENGTH_ERROR                            = 0x1A00,
			PARAMETER_NOT_SUPPORTED                                = 0x2601,
			PARAMETER_VALUE_INVALID                                = 0x2602,
			PARAMETERS_CHANGED                                     = 0x2A00,
			PERIPHERAL_DEVICE_WRITE_FAULT                          = 0x0300,
			POSITIONING_ERROR_DETECTED_BY_READ_OF_MEDIUM           = 0x1502,
			POWER_ON_RESET_OR_BUS_DEVICE_RESET_OCCURRED            = 0x2900,
			POWER_ON_RESET                                         = 0x2901,
			POWER_ON_OR_SELF_TEST_FAILURE                          = 0x4200,
			PRIMARY_DEFECT_LIST_NOT_FOUND                          = 0x1C01,
			RAM_FAILURE                                            = 0x4000,
			RANDOM_POSITIONING_ERROR                               = 0x1500,
			READ_RETRIES_EXHAUSTED                                 = 0x1101,
			RECORD_NOT_FOUND                                       = 0x1401,
			RECORDED_ENTITY_NOT_FOUND                              = 0x1400,
			RECOVERED_DATA_DATA_AUTO_REALLOCATED                   = 0x1802,
			RECOVERED_DATA_RECOMMEND_REASSIGNMENT                  = 0x1805,
			RECOVERED_DATA_RECOMMEND_REWRITE                       = 0x1806,
			RECOVERED_DATA_USING_PREVIOUS_SECTOR_ID                = 0x1705,
			RECOVERED_DATA_WITH_ERROR_CORRECTION_RETRIES_APPLIED   = 0x1801,
			RECOVERED_DATA_WITH_ERROR_CORRECTION_APPLIED           = 0x1800,
			RECOVERED_DATA_WITH_NEGATIVE_HEAD_OFFSET               = 0x1703,
			RECOVERED_DATA_WITH_NO_ERROR_CORRECTION_APPLIED        = 0x1700,
			RECOVERED_DATA_WITH_POSITIVE_HEAD_OFFSET               = 0x1702,
			RECOVERED_DATA_WITH_RETRIES                            = 0x1701,
			RECOVERED_DATA_WITHOUT_ECC_DATA_AUTO_REALLOCATED       = 0x1706,
			RECOVERED_DATA_WITHOUT_ECC_RECOMMEND_REASSIGNMENT      = 0x1707,
			RECOVERED_DATA_WITHOUT_ECC_RECOMMEND_REWRITE           = 0x1708,
			RECOVERED_ID_WITH_ECC_CORRECTION                       = 0x1E00,
			ROUNDED_PARAMETER                                      = 0x3700,
			RPL_STATUS_CHANGE                                      = 0x5C00,
			SAVING_PARAMETERS_NOT_SUPPORTED                        = 0x3900,
			SCSI_BUS_RESET                                         = 0x2902,
			SCSI_PARITY_ERROR                                      = 0x4700,
			SELECT_OR_RESELECT_FAILURE                             = 0x4500,
			SPINDLES_NOT_SYNCHRONIZED                              = 0x5C02,
			SPINDLES_SYNCHRONIZED                                  = 0x5C01,
			SYNCHRONOUS_DATA_TRANSFER_ERROR                        = 0x1B00,
			TARGET_OPERATING_CONDITIONS_HAVE_CHANGED               = 0x3F00,
			THRESHOLD_CONDITION_MET                                = 0x5B01,
			THRESHOLD_PARAMETERS_NOT_SUPPORTED                     = 0x2603,
			TRACK_FOLLOWING_ERROR                                  = 0x0900,
			UNRECOVERED_READ_ERROR                                 = 0x1100,
			UNRECOVERED_READ_ERROR_AUTO_REALLOCATE_FAILED          = 0x1104,
			UNRECOVERED_READ_ERROR_RECOMMEND_REASSIGNMENT          = 0x110B,
			UNRECOVERED_READ_ERROR_RECOMMEND_REWRITE_THE_DATA      = 0x110C,
			UNSUCCESSFUL_SOFT_RESET                                = 0x4600,
			WRITE_ERROR_AUTO_REALLOCATION_FAILED                   = 0x0C02,
			WRITE_ERROR_RECOVERED_WITH_AUTO_REALLOCATION           = 0x0C01,
			WRITE_PROTECTED                                        = 0x2700
		} SCSI_ASC_ASCQ;
		typedef enum
		{
			DISK_STARTED = 1,     // Controlled via START STOP UNIT
			DISK_PRESENT = 2,     // SD card is physically present
			DISK_INITIALISED = 4, // SD card responded to init sequence
			DISK_WP = 8           // Write-protect.
		} DISK_STATE;
		typedef enum
		{
			// internal bits
			__scsiphase_msg = 1,
			__scsiphase_cd = 2,
			__scsiphase_io = 4,

			BUS_FREE = -1,
			BUS_BUSY = -2,
			ARBITRATION = -3,
			SELECTION = -4,
			RESELECTION = -5,
			STATUS = __scsiphase_cd | __scsiphase_io,
			COMMAND = __scsiphase_cd,
			DATA_IN = __scsiphase_io,
			DATA_OUT = 0,
			MESSAGE_IN = __scsiphase_msg | __scsiphase_cd | __scsiphase_io,
			MESSAGE_OUT = __scsiphase_msg | __scsiphase_cd
		} SCSI_PHASE;
#define MSG(a) case a: msg << #a; break;
#define MSGBIT(a,b) if (a & b) { msg << #b; msg << " "; }

		if (buf.size() == 0)
			return;

		ticks = buf[25];
		buf[25] = 0;
		if (last == buf)
			return;
		last = buf;
		switch (buf[0]) {
		case TEST_UNIT_READY:
			msg << "TEST_UNIT_READY";
			break;
		case REZERO_UNIT:  msg << "REZERO_UNIT"; break;
		case REQUEST_SENSE:
			msg << "REQUEST_SENSE";
			msg << " LENGTH " << (int)(buf[4]);
			break;
		case FORMAT_UNIT:
			msg << "FORMAT_UNIT";
			msg << " DATA " << (buf[1] & 0x10);
			break;
		case READ_BLOCK_LIMITS:  msg << "READ_BLOCK_LIMITS"; break;
		case REASSIGN_BLOCKS:  msg << "REASSIGN_BLOCKS"; break;
		case READ_6:
			msg << "READ_6";
			msg << " LBA " << (((int) buf[1] & 0x1F) << 16) +
                                          (((int) buf[2]) << 8) + buf[3];
			msg << " BLOCKS " << (int)(buf[4]);
			break;
		case WRITE_6:
			msg << "WRITE_6";
			msg << " LBA " << (((uint32_t) buf[1] & 0x1F) << 16) +
                                          (((uint32_t) buf[2]) << 8) + buf[3];
			msg << " BLOCKS " << (int)(buf[4]);
			break;
		case SEEK_6:
			msg << "SEEK_6";
			msg << " LBA " << (((uint32_t) buf[1] & 0x1F) << 16) +
                                          (((uint32_t) buf[2]) << 8) + buf[3];
			break;
		case READ_REVERSE:  msg << "READ_REVERSE"; break;
		case WRITE_FILEMARKS:  msg << "WRITE_FILEMARKS"; break;
		case SPACE:  msg << "SPACE"; break;
		case INQUIRY:
			msg << "INQUIRY";
			msg << " ENABLE VPD " << (buf[1] & 1);
			msg << " PAGE CODE " << (int)(buf[2]);
			msg << " LENGTH " << (int)(buf[4]);
			break;
		case RECOVER_BUFFERED_DATA:  msg << "RECOVER_BUFFERED_DATA"; break;
		case MODE_SELECT:
			msg << "MODE_SELECT";
			msg << " LENGTH " << (int)(buf[4]);
			break;
		case RESERVE:  msg << "RESERVE"; break;
		case RELEASE:  msg << "RELEASE"; break;
		case COPY:  msg << "COPY"; break;
		case ERASE:  msg << "ERASE"; break;
		case MODE_SENSE:
			msg << "MODE_SENSE";
			msg << " DBD " << (buf[1] & 0x08);
			msg << " PAGE CONTROL " << (buf[2] >> 6);
			msg << " PAGE CODE " << (buf[2] & 0x3F);
			msg << " LENGTH " << (int)(buf[4]);
			break;
		case START_STOP:
			msg << "START_STOP";
			msg << " IMMED " << (buf[1] & 1);
			msg << " START " << (buf[4] & 1);
			break;
		case RECEIVE_DIAGNOSTIC:  msg << "RECEIVE_DIAGNOSTIC"; break;
		case SEND_DIAGNOSTIC:  msg << "SEND_DIAGNOSTIC"; break;
		case ALLOW_MEDIUM_REMOVAL:  msg << "ALLOW_MEDIUM_REMOVAL"; break;
		case SET_WINDOW:  msg << "SET_WINDOW"; break;
		case READ_CAPACITY:
			msg << "READ_CAPACITY";
			msg << " LBA " << (((uint32_t)buf[2]) << 24) +
					  (((uint32_t)buf[3]) << 16) +
					  (((uint32_t)buf[4]) << 8) + buf[5];
			msg << " PMI " << (buf[8] & 1);
			break;
		case READ_10:
			msg << "READ_10";
			msg << " LBA " << (((uint32_t)buf[2]) << 24) + (((uint32_t)buf[3]) << 16) +
					  (((uint32_t) buf[4]) << 8) + buf[5];
			msg << " BLOCKS " << (((uint32_t) buf[7]) << 8) + buf[8];
			break;
		case WRITE_10:
			msg << "WRITE_10";
			msg << " LBA " << (((uint32_t)buf[2]) << 24) + (((uint32_t)buf[3]) << 16) +
					  (((uint32_t) buf[4]) << 8) + buf[5];
			msg << " BLOCKS " << (((uint32_t) buf[7]) << 8) + buf[8];
			break;
		case SEEK_10:
			msg << "SEEK_10";
			msg << " LBA " << (((uint32_t)buf[2]) << 24) + (((uint32_t)buf[3]) << 16) +
					  (((uint32_t) buf[4]) << 8) + buf[5];
			break;
		case WRITE_VERIFY:
			msg << "WRITE_VERIFY";
			msg << " LBA " << (((uint32_t)buf[2]) << 24) + (((uint32_t)buf[3]) << 16) +
					  (((uint32_t) buf[4]) << 8) + buf[5];
			msg << " BLOCKS " << (((uint32_t) buf[7]) << 8) + buf[8];
			break;
		case VERIFY:  msg << "VERIFY"; break;
		case SEARCH_HIGH:  msg << "SEARCH_HIGH"; break;
		case SEARCH_EQUAL:  msg << "SEARCH_EQUAL"; break;
		case SEARCH_LOW:  msg << "SEARCH_LOW"; break;
		case SET_LIMITS:  msg << "SET_LIMITS"; break;
		case READ_POSITION:  msg << "READ_POSITION"; break;
		case SYNCHRONIZE_CACHE:  msg << "SYNCHRONIZE_CACHE"; break;
		case LOCK_UNLOCK_CACHE:  msg << "LOCK_UNLOCK_CACHE"; break;
		case READ_DEFECT_DATA:  msg << "READ_DEFECT_DATA"; break;
		case MEDIUM_SCAN:  msg << "MEDIUM_SCAN"; break;
		case COMPARE:  msg << "COMPARE"; break;
		case COPY_VERIFY:  msg << "COPY_VERIFY"; break;
		case WRITE_BUFFER:  msg << "WRITE_BUFFER"; break;
		case READ_BUFFER:  msg << "READ_BUFFER"; break;
		case UPDATE_BLOCK:  msg << "UPDATE_BLOCK"; break;
		case READ_LONG:  msg << "READ_LONG"; break;
		case WRITE_LONG:  msg << "WRITE_LONG"; break;
		case CHANGE_DEFINITION:  msg << "CHANGE_DEFINITION"; break;
		case WRITE_SAME:  msg << "WRITE_SAME"; break;
		case READ_TOC:
			msg << "READ_TOC";
			msg << " MSF " << (buf[1] & 0x02);
			msg << " TRACK " << (int)(buf[6]);
			msg << " LENGTH " <<  (((uint32_t) buf[7]) << 8) + buf[8];
			msg << " FORMAT " << (buf[2] & 0x0F);
			break;
		case REPORT_DENSITY:
			msg << "REPORT_DENSITY";
			msg << " MSF " << (buf[1] & 0x02);
			msg << " LENGTH " <<  (((uint32_t) buf[7]) << 8) + buf[8];
			break;
		case LOG_SELECT:  msg << "LOG_SELECT"; break;
		case LOG_SENSE:  msg << "LOG_SENSE"; break;
		case MODE_SELECT_10:
			msg << "MODE_SELECT_10";
			msg << " LENGTH " << (((uint16_t) buf[7]) << 8) + buf[8];
			break;
		case MODE_SENSE_10:
			msg << "MODE_SENSE_10";
			msg << " DBD " << (buf[1] & 0x08);
			msg << " PAGE CONTROL " << (buf[2] >> 6);
			msg << " PAGE CODE " << (buf[2] & 0x3F);
			msg << " LENGTH " << (((uint16_t) buf[7]) << 8) + buf[8];
			break;
		case MOVE_MEDIUM:  msg << "MOVE_MEDIUM"; break;
		case READ_12:  msg << "READ_12"; break;
		case WRITE_12:  msg << "WRITE_12"; break;
		case WRITE_VERIFY_12:  msg << "WRITE_VERIFY_12"; break;
		case SEARCH_HIGH_12:  msg << "SEARCH_HIGH_12"; break;
		case SEARCH_EQUAL_12:  msg << "SEARCH_EQUAL_12"; break;
		case SEARCH_LOW_12:  msg << "SEARCH_LOW_12"; break;
		case READ_ELEMENT_STATUS:  msg << "READ_ELEMENT_STATUS"; break;
		case SEND_VOLUME_TAG:  msg << "SEND_VOLUME_TAG"; break;
		case WRITE_LONG_2:  msg << "WRITE_LONG_2"; break;
		default:
			msg << "UNKNOWN(" << (int)(buf[0]) << ")";
			break;
		}
		msg << "\n          ";
		for (size_t i = 0; i < 12 && i < buf.size(); ++i)
		{
			msg << std::setfill('0') << std::setw(2) <<
			static_cast<int>(buf[i]) << ' ';
		}
		msg << "\n          ";
		switch ((char)buf[16]) {
		MSG(BUS_FREE)
		MSG(BUS_BUSY)
		MSG(ARBITRATION)
		MSG(SELECTION)
		MSG(RESELECTION)
		MSG(STATUS)
		MSG(COMMAND)
		MSG(DATA_IN)
		MSG(DATA_OUT)
		MSG(MESSAGE_IN)
		MSG(MESSAGE_OUT)
		}
		msg << " ";
		switch (buf[14]) {
		MSG(GOOD)
		MSG(CHECK_CONDITION)
		MSG(BUSY)
		MSG(INTERMEDIATE)
		MSG(CONFLICT)
		}
		if (buf[17])
			msg << " BSY";
		if (buf[18])
			msg << " SEL";
		if (buf[19])
			msg << " ATN";
		if (buf[20])
			msg << " RST";
		msg << " DBx " << (int)(buf[29]);
		msg << "\n          ";
		switch (buf[15]) {
		MSG(NO_SENSE)
		MSG(RECOVERED_ERROR)
		MSG(NOT_READY)
		MSG(MEDIUM_ERROR)
		MSG(HARDWARE_ERROR)
		MSG(ILLEGAL_REQUEST)
		MSG(UNIT_ATTENTION)
		MSG(DATA_PROTECT)
		MSG(BLANK_CHECK)
		MSG(VENDOR_SPECIFIC)
		MSG(COPY_ABORTED)
		MSG(ABORTED_COMMAND)
		MSG(EQUAL)
		MSG(VOLUME_OVERFLOW)
		MSG(MISCOMPARE)
		MSG(RESERVED)
		}
		msg << " ";
		switch (((uint16_t)buf[27] << 8) + buf[28]) {
		MSG(ADDRESS_MARK_NOT_FOUND_FOR_DATA_FIELD)
		MSG(ADDRESS_MARK_NOT_FOUND_FOR_ID_FIELD)
		MSG(CANNOT_READ_MEDIUM_INCOMPATIBLE_FORMAT)
		MSG(CANNOT_READ_MEDIUM_UNKNOWN_FORMAT)
		MSG(CHANGED_OPERATING_DEFINITION)
		MSG(COMMAND_PHASE_ERROR)
		MSG(COMMAND_SEQUENCE_ERROR)
		MSG(COMMANDS_CLEARED_BY_ANOTHER_INITIATOR)
		MSG(COPY_CANNOT_EXECUTE_SINCE_HOST_CANNOT_DISCONNECT)
		MSG(DATA_PATH_FAILURE)
		MSG(DATA_PHASE_ERROR)
		MSG(DATA_SYNCHRONIZATION_MARK_ERROR)
		MSG(DEFECT_LIST_ERROR)
		MSG(DEFECT_LIST_ERROR_IN_GROWN_LIST)
		MSG(DEFECT_LIST_ERROR_IN_PRIMARY_LIST)
		MSG(DEFECT_LIST_NOT_AVAILABLE)
		MSG(DEFECT_LIST_NOT_FOUND)
		MSG(DEFECT_LIST_UPDATE_FAILURE)
		MSG(ERROR_LOG_OVERFLOW)
		MSG(ERROR_TOO_LONG_TO_CORRECT)
		MSG(FORMAT_COMMAND_FAILED)
		MSG(GROWN_DEFECT_LIST_NOT_FOUND)
		MSG(IO_PROCESS_TERMINATED)
		MSG(ID_CRC_OR_ECC_ERROR)
		MSG(ILLEGAL_FUNCTION)
		MSG(INCOMPATIBLE_MEDIUM_INSTALLED)
		MSG(INITIATOR_DETECTED_ERROR_MESSAGE_RECEIVED)
		MSG(INQUIRY_DATA_HAS_CHANGED)
		MSG(INTERNAL_TARGET_FAILURE)
		MSG(INVALID_BITS_IN_IDENTIFY_MESSAGE)
		MSG(INVALID_COMMAND_OPERATION_CODE)
		MSG(INVALID_FIELD_IN_CDB)
		MSG(INVALID_FIELD_IN_PARAMETER_LIST)
		MSG(INVALID_MESSAGE_ERROR)
		MSG(LOG_COUNTER_AT_MAXIMUM)
		MSG(LOG_EXCEPTION)
		MSG(LOG_LIST_CODES_EXHAUSTED)
		MSG(LOG_PARAMETERS_CHANGED)
		MSG(LOGICAL_BLOCK_ADDRESS_OUT_OF_RANGE)
		MSG(LOGICAL_UNIT_COMMUNICATION_FAILURE)
		MSG(LOGICAL_UNIT_COMMUNICATION_PARITY_ERROR)
		MSG(LOGICAL_UNIT_COMMUNICATION_TIMEOUT)
		MSG(LOGICAL_UNIT_DOES_NOT_RESPOND_TO_SELECTION)
		MSG(LOGICAL_UNIT_FAILED_SELF_CONFIGURATION)
		MSG(LOGICAL_UNIT_HAS_NOT_SELF_CONFIGURED_YET)
		MSG(LOGICAL_UNIT_IS_IN_PROCESS_OF_BECOMING_READY)
		MSG(LOGICAL_UNIT_NOT_READY_CAUSE_NOT_REPORTABLE)
		MSG(LOGICAL_UNIT_NOT_READY_FORMAT_IN_PROGRESS)
		MSG(LOGICAL_UNIT_NOT_READY_INITIALIZING_COMMAND_REQUIRED)
		MSG(LOGICAL_UNIT_NOT_READY_MANUAL_INTERVENTION_REQUIRED)
		MSG(LOGICAL_UNIT_NOT_SUPPORTED)
		MSG(MECHANICAL_POSITIONING_ERROR)
		MSG(MEDIA_LOAD_OR_EJECT_FAILED)
		MSG(MEDIUM_FORMAT_CORRUPTED)
		MSG(MEDIUM_NOT_PRESENT)
		MSG(MEDIUM_REMOVAL_PREVENTED)
		MSG(MESSAGE_ERROR)
		MSG(MICROCODE_HAS_BEEN_CHANGED)
		MSG(MISCOMPARE_DURING_VERIFY_OPERATION)
		MSG(MISCORRECTED_ERROR)
		MSG(MODE_PARAMETERS_CHANGED)
		MSG(MULTIPLE_PERIPHERAL_DEVICES_SELECTED)
		MSG(MULTIPLE_READ_ERRORS)
		MSG(NO_ADDITIONAL_SENSE_INFORMATION)
		MSG(NO_DEFECT_SPARE_LOCATION_AVAILABLE)
		MSG(NO_INDEX_SECTOR_SIGNAL)
		MSG(NO_REFERENCE_POSITION_FOUND)
		MSG(NO_SEEK_COMPLETE)
		MSG(NOT_READY_TO_READY_TRANSITION_MEDIUM_MAY_HAVE_CHANGED)
		MSG(OPERATOR_MEDIUM_REMOVAL_REQUEST)
		MSG(OPERATOR_REQUEST_OR_STATE_CHANGE_INPUT)
		MSG(OPERATOR_SELECTED_WRITE_PERMIT)
		MSG(OPERATOR_SELECTED_WRITE_PROTECT)
		MSG(OVERLAPPED_COMMANDS_ATTEMPTED)
		MSG(PARAMETER_LIST_LENGTH_ERROR)
		MSG(PARAMETER_NOT_SUPPORTED)
		MSG(PARAMETER_VALUE_INVALID)
		MSG(PARAMETERS_CHANGED)
		MSG(PERIPHERAL_DEVICE_WRITE_FAULT)
		MSG(POSITIONING_ERROR_DETECTED_BY_READ_OF_MEDIUM)
		MSG(POWER_ON_RESET_OR_BUS_DEVICE_RESET_OCCURRED)
		MSG(POWER_ON_RESET)
		MSG(POWER_ON_OR_SELF_TEST_FAILURE)
		MSG(PRIMARY_DEFECT_LIST_NOT_FOUND)
		MSG(RAM_FAILURE)
		MSG(RANDOM_POSITIONING_ERROR)
		MSG(READ_RETRIES_EXHAUSTED)
		MSG(RECORD_NOT_FOUND)
		MSG(RECORDED_ENTITY_NOT_FOUND)
		MSG(RECOVERED_DATA_DATA_AUTO_REALLOCATED)
		MSG(RECOVERED_DATA_RECOMMEND_REASSIGNMENT)
		MSG(RECOVERED_DATA_RECOMMEND_REWRITE)
		MSG(RECOVERED_DATA_USING_PREVIOUS_SECTOR_ID)
		MSG(RECOVERED_DATA_WITH_ERROR_CORRECTION_RETRIES_APPLIED)
		MSG(RECOVERED_DATA_WITH_ERROR_CORRECTION_APPLIED)
		MSG(RECOVERED_DATA_WITH_NEGATIVE_HEAD_OFFSET)
		MSG(RECOVERED_DATA_WITH_NO_ERROR_CORRECTION_APPLIED)
		MSG(RECOVERED_DATA_WITH_POSITIVE_HEAD_OFFSET)
		MSG(RECOVERED_DATA_WITH_RETRIES)
		MSG(RECOVERED_DATA_WITHOUT_ECC_DATA_AUTO_REALLOCATED)
		MSG(RECOVERED_DATA_WITHOUT_ECC_RECOMMEND_REASSIGNMENT)
		MSG(RECOVERED_DATA_WITHOUT_ECC_RECOMMEND_REWRITE)
		MSG(RECOVERED_ID_WITH_ECC_CORRECTION)
		MSG(ROUNDED_PARAMETER)
		MSG(RPL_STATUS_CHANGE)
		MSG(SAVING_PARAMETERS_NOT_SUPPORTED)
		MSG(SCSI_BUS_RESET)
		MSG(SCSI_PARITY_ERROR)
		MSG(SELECT_OR_RESELECT_FAILURE)
		MSG(SPINDLES_NOT_SYNCHRONIZED)
		MSG(SPINDLES_SYNCHRONIZED)
		MSG(SYNCHRONOUS_DATA_TRANSFER_ERROR)
		MSG(TARGET_OPERATING_CONDITIONS_HAVE_CHANGED)
		MSG(THRESHOLD_CONDITION_MET)
		MSG(THRESHOLD_PARAMETERS_NOT_SUPPORTED)
		MSG(TRACK_FOLLOWING_ERROR)
		MSG(UNRECOVERED_READ_ERROR)
		MSG(UNRECOVERED_READ_ERROR_AUTO_REALLOCATE_FAILED)
		MSG(UNRECOVERED_READ_ERROR_RECOMMEND_REASSIGNMENT)
		MSG(UNRECOVERED_READ_ERROR_RECOMMEND_REWRITE_THE_DATA)
		MSG(UNSUCCESSFUL_SOFT_RESET)
		MSG(WRITE_ERROR_AUTO_REALLOCATION_FAILED)
		MSG(WRITE_ERROR_RECOVERED_WITH_AUTO_REALLOCATION)
		MSG(WRITE_PROTECTED)
		}
		msg << "\n          ";
		MSGBIT(buf[26], DISK_STARTED)
		MSGBIT(buf[26], DISK_PRESENT)
		MSGBIT(buf[26], DISK_INITIALISED)
		MSGBIT(buf[26], DISK_WP)
		msg << "\n          ";
		msg << "Ticks " << ticks;
		msg << " msgIn " << (int)(buf[12]);
		msg << " msgOut " << (int)(buf[13]);
		msg << " rstCount " << (int)(buf[21]);
		msg << " selCount " << (int)(buf[22]);
		msg << " msgCount " << (int)(buf[23]);
		msg << " cmdCount " << (int)(buf[24]);
		wxLogMessage(this, msg.str().c_str());
        }

	void logSCSI()
	{
		if (!mySCSILogChk->IsChecked() ||
			!myHID)
		{
			return;
		}
		try
		{
			std::vector<uint8_t> info(HID::HID_PACKET_SIZE);
			if (myHID->readSCSIDebugInfo(info))
			{
				dumpSCSICommand(info);
			}
		}
		catch (std::exception& e)
		{
			wxLogWarning(this, e.what());
			myHID.reset();
		}
	}

	void OnID_Timer(wxTimerEvent& event)
	{
		logSCSI();
		time_t now = time(NULL);
		if (now == myLastPollTime) return;
		myLastPollTime = now;

		// Check if we are connected to the HID device.
		// AND/or bootloader device.
		try
		{
			if (myBootloader)
			{
				// Verify the USB HID connection is valid
				if (!myBootloader->ping())
				{
					myBootloader.reset();
				}
			}

			if (!myBootloader)
			{
				myBootloader.reset(Bootloader::Open());

				if (myBootloader)
				{
					mmLogStatus("SCSI2SD Bootloader Ready");
				}
			}

			int supressLog = 0;
			if (myHID && myHID->getFirmwareVersion() < MIN_FIRMWARE_VERSION)
			{
				// No method to check connection is still valid.
				// So assume it isn't.
				myHID.reset();
				supressLog = 1;
			}
			else if (myHID && !myHID->ping())
			{
				// Verify the USB HID connection is valid
				myHID.reset();
			}

			if (!myHID)
			{
				myHID.reset(HID::Open());
				if (myHID)
				{
					if (myHID->getFirmwareVersion() < MIN_FIRMWARE_VERSION)
					{
						if (!supressLog)
						{
							// Oh dear, old firmware
							std::stringstream msg;
							msg << "Firmware update required. Version " <<
								myHID->getFirmwareVersionStr();
							mmLogStatus(msg.str());
						}
					}
					else
					{
						std::stringstream msg;
						msg << "SCSI2SD Ready, firmware version " <<
							myHID->getFirmwareVersionStr();
						mmLogStatus(msg.str());

						std::vector<uint8_t> csd(myHID->getSD_CSD());
						std::vector<uint8_t> cid(myHID->getSD_CID());
						std::stringstream sdinfo;
						sdinfo << "SD Capacity (512-byte sectors): " <<
							myHID->getSDCapacity() << std::endl;

						sdinfo << "SD CSD Register: ";
						if (sdCrc7(&csd[0], 15, 0) != (csd[15] >> 1))
						{
							sdinfo << "BADCRC ";
						}
						for (size_t i = 0; i < csd.size(); ++i)
						{
							sdinfo <<
								std::hex << std::setfill('0') << std::setw(2) <<
								static_cast<int>(csd[i]);
						}
						sdinfo << std::endl;
						sdinfo << "SD CID Register: ";
						if (sdCrc7(&cid[0], 15, 0) != (cid[15] >> 1))
						{
							sdinfo << "BADCRC ";
						}
						for (size_t i = 0; i < cid.size(); ++i)
						{
							sdinfo <<
								std::hex << std::setfill('0') << std::setw(2) <<
								static_cast<int>(cid[i]);
						}

						wxLogMessage(this, "%s", sdinfo.str());

						if (mySelfTestChk->IsChecked())
						{
							std::stringstream scsiInfo;
							scsiInfo << "SCSI Self-Test: " <<
								(myHID->scsiSelfTest() ? "Passed" : "FAIL");
							wxLogMessage(this, "%s", scsiInfo.str());
						}

						if (!myInitialConfig)
						{
/* This doesn't work properly, and causes crashes.
							wxCommandEvent loadEvent(wxEVT_NULL, ID_BtnLoad);
							GetEventHandler()->AddPendingEvent(loadEvent);
*/
						}

					}
				}
				else
				{
					char ticks[] = {'/', '-', '\\', '|'};
					std::stringstream ss;
					ss << "Searching for SCSI2SD device " << ticks[myTickCounter % sizeof(ticks)];
					myTickCounter++;
					SetStatusText(ss.str());
				}
			}
		}
		catch (std::runtime_error& e)
		{
			std::cerr << e.what() << std::endl;
			mmLogStatus(e.what());
		}

		evaluate();
	}

	void doLoad(wxCommandEvent& event)
	{
		TimerLock lock(myTimer);
		if (!myHID) return;

		mmLogStatus("Loading configuration");

		wxWindowPtr<wxGenericProgressDialog> progress(
			new wxGenericProgressDialog(
				"Load config settings",
				"Loading config settings",
				100,
				this,
				wxPD_CAN_ABORT | wxPD_REMAINING_TIME)
				);

		int flashRow = SCSI_CONFIG_0_ROW;
		int currentProgress = 0;
		int totalProgress = myTargets.size() * SCSI_CONFIG_ROWS;
		for (size_t i = 0;
			i < myTargets.size();
			++i, flashRow += SCSI_CONFIG_ROWS)
		{
			std::vector<uint8_t> raw(sizeof(TargetConfig));

			for (size_t j = 0; j < SCSI_CONFIG_ROWS; ++j)
			{
				std::stringstream ss;
				ss << "Reading flash array " << SCSI_CONFIG_ARRAY <<
					" row " << (flashRow + j);
				mmLogStatus(ss.str());
				currentProgress += 1;
				if (currentProgress == totalProgress)
				{
					ss.str("Load Complete.");
					mmLogStatus("Load Complete.");
				}

				if (!progress->Update(
						(100 * currentProgress) / totalProgress,
						ss.str()
						)
					)
				{
					goto abort;
				}

				std::vector<uint8_t> flashData;

				try
				{
					myHID->readFlashRow(
						SCSI_CONFIG_ARRAY, flashRow + j, flashData);

				}
				catch (std::runtime_error& e)
				{
					mmLogStatus(e.what());
					goto err;
				}

				std::copy(
					flashData.begin(),
					flashData.end(),
					&raw[j * SCSI_CONFIG_ROW_SIZE]);
			}
			myTargets[i]->setConfig(ConfigUtil::fromBytes(&raw[0]));
		}

		myInitialConfig = true;
		goto out;

	err:
		mmLogStatus("Load failed");
		progress->Update(100, "Load failed");
		goto out;

	abort:
		mmLogStatus("Load Aborted");

	out:
		return;
	}

	void doSave(wxCommandEvent& event)
	{
		TimerLock lock(myTimer);
		if (!myHID) return;

		mmLogStatus("Saving configuration");

		wxWindowPtr<wxGenericProgressDialog> progress(
			new wxGenericProgressDialog(
				"Save config settings",
				"Saving config settings",
				100,
				this,
				wxPD_CAN_ABORT | wxPD_REMAINING_TIME)
				);

		int flashRow = SCSI_CONFIG_0_ROW;
		int currentProgress = 0;
		int totalProgress = myTargets.size() * SCSI_CONFIG_ROWS;
		for (size_t i = 0;
			i < myTargets.size();
			++i, flashRow += SCSI_CONFIG_ROWS)
		{
			TargetConfig config(myTargets[i]->getConfig());
			std::vector<uint8_t> raw(ConfigUtil::toBytes(config));

			for (size_t j = 0; j < SCSI_CONFIG_ROWS; ++j)
			{
				std::stringstream ss;
				ss << "Programming flash array " << SCSI_CONFIG_ARRAY <<
					" row " << (flashRow + j);
				mmLogStatus(ss.str());
				currentProgress += 1;

				if (currentProgress == totalProgress)
				{
					ss.str("Save Complete.");
					mmLogStatus("Save Complete.");
				}
				if (!progress->Update(
						(100 * currentProgress) / totalProgress,
						ss.str()
						)
					)
				{
					goto abort;
				}

				std::vector<uint8_t> flashData(SCSI_CONFIG_ROW_SIZE, 0);
				std::copy(
					&raw[j * SCSI_CONFIG_ROW_SIZE],
					&raw[(1+j) * SCSI_CONFIG_ROW_SIZE],
					flashData.begin());
				try
				{
					myHID->writeFlashRow(
						SCSI_CONFIG_ARRAY, flashRow + j, flashData);
				}
				catch (std::runtime_error& e)
				{
					mmLogStatus(e.what());
					goto err;
				}
			}
		}

		// Reboot so new settings take effect.
		myHID->enterBootloader();
		myHID.reset();


		goto out;

	err:
		mmLogStatus("Save failed");
		progress->Update(100, "Save failed");
		goto out;

	abort:
		mmLogStatus("Save Aborted");

	out:
		return;
	}

	// Note: Don't confuse this with the wxApp::OnExit virtual method
	void OnExitEvt(wxCommandEvent& event);

	void OnCloseEvt(wxCloseEvent& event);

	void OnAbout(wxCommandEvent& event)
	{
		wxMessageBox(
			"SCSI2SD (scsi2sd-util)\n"
			"Copyright (C) 2014 Michael McMaster <michael@codesrc.com>\n"
			"\n"
"This program is free software: you can redistribute it and/or modify\n"
"it under the terms of the GNU General Public License as published by\n"
"the Free Software Foundation, either version 3 of the License, or\n"
"(at your option) any later version.\n"
"\n"
"This program is distributed in the hope that it will be useful,\n"
"but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
"GNU General Public License for more details.\n"
"\n"
"You should have received a copy of the GNU General Public License\n"
"along with this program.  If not, see <http://www.gnu.org/licenses/>.\n",

			"About scsi2sd-util", wxOK | wxICON_INFORMATION );
	}

	wxDECLARE_EVENT_TABLE();
};

wxBEGIN_EVENT_TABLE(AppFrame, wxFrame)
	EVT_MENU(AppFrame::ID_ConfigDefaults, AppFrame::OnID_ConfigDefaults)
	EVT_MENU(AppFrame::ID_Firmware, AppFrame::OnID_Firmware)
	EVT_MENU(AppFrame::ID_LogWindow, AppFrame::OnID_LogWindow)
	EVT_MENU(AppFrame::ID_SaveFile, AppFrame::OnID_SaveFile)
	EVT_MENU(AppFrame::ID_OpenFile, AppFrame::OnID_OpenFile)
	EVT_MENU(wxID_EXIT, AppFrame::OnExitEvt)
	EVT_MENU(wxID_ABOUT, AppFrame::OnAbout)

	EVT_TIMER(AppFrame::ID_Timer, AppFrame::OnID_Timer)

	EVT_COMMAND(wxID_ANY, ConfigChangedEvent, AppFrame::onConfigChanged)

	EVT_BUTTON(ID_BtnSave, AppFrame::doSave)
	EVT_BUTTON(ID_BtnLoad, AppFrame::doLoad)

	EVT_CLOSE(AppFrame::OnCloseEvt)

wxEND_EVENT_TABLE()



class App : public wxApp
{
public:
	virtual bool OnInit()
	{
		AppFrame* frame = new AppFrame();
		frame->Show(true);
		SetTopWindow(frame);
		return true;
	}
};
} // namespace

// Main Method
wxIMPLEMENT_APP(App);

void
AppFrame::OnExitEvt(wxCommandEvent& event)
{
	wxGetApp().ExitMainLoop();
}

void
AppFrame::OnCloseEvt(wxCloseEvent& event)
{
	wxGetApp().ExitMainLoop();
}

