/*=========================================================================

  Program:   ITK-SNAP
  Module:    DistributedSegmentationModel.cxx
  Language:  C++
  Date:      March 2018

  Copyright (c) 2018 Paul A. Yushkevich

  This file is part of ITK-SNAP

  ITK-SNAP is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

  -----

  Copyright (c) 2003 Insight Software Consortium. All rights reserved.
  See ITKCopyright.txt or http://www.itk.org/HTML/Copyright.htm for details.

  This software is distributed WITHOUT ANY WARRANTY; without even
  the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
  PURPOSE.  See the above copyright notices for more information.

=========================================================================*/
#include "DistributedSegmentationModel.h"

#include "GlobalUIModel.h"
#include "RESTClient.h"
#include "FormattedTable.h"
#include "IRISApplication.h"
#include "IRISImageData.h"
#include "WorkspaceAPI.h"
#include "json/json.h"
#include "UIReporterDelegates.h"
#include <sstream>
#include <algorithm>
#include "itksys/SystemTools.hxx"

namespace dss_model {

/** TODO: make this sort versions properly */
bool service_summary_cmp(const ServiceSummary &a, const ServiceSummary &b)
{
  if(a.name < b.name)
    return true;

  if(a.name == b.name && a.version < b.version)
    return true;

  return false;
}

bool TagSpec::operator ==(const TagSpec &o) const
{
  return
      name == o.name && type == o.type &&
      required == o.required && hint == o.hint &&
      object_id == o.object_id;
}

const char *ticket_status_emap_initializer[] =
{
  "init", "ready", "claimed", "success", "failed", "timeout", "unknown", NULL
};
RegistryEnumMap<TicketStatus> ticket_status_emap(ticket_status_emap_initializer);

const char *log_type_emap_initializer[] =
{
  "info", "warning", "error", "unknown", NULL
};
RegistryEnumMap<LogType> log_type_emap(log_type_emap_initializer);


std::string ticket_status_strings[] =
{
  "initialized", "ready", "claimed", "success", "failed", "timed out"
};

std::string tag_type_strings[] =
{
  "Image Layer", "Main Image", "Overlay Image", "Segmentation Label", "Point Landmark", "Unknown"
};


} // namespace

using namespace dss_model;

void DistributedSegmentationModel::SetParentModel(GlobalUIModel *model)
{
  m_Parent = model;
}

void DistributedSegmentationModel::LoadPreferences(Registry &folder)
{
  // Read the list of servers
  std::vector<std::string> user_servers = folder.Folder("UserServerList").GetArray(std::string());
  this->SetUserServerList(user_servers);

  // Read the preferred server
  int pref_server = folder["PreferredServerIndex"][0];
  if(pref_server < m_ServerURLList.size())
    m_ServerURLModel->SetValue(pref_server);
}

void DistributedSegmentationModel::SavePreferences(Registry &folder)
{
  // Save the list of servers
  std::vector<std::string> user_servers = this->GetUserServerList();
  folder.Folder("UserServerList").PutArray(user_servers);

  // Save the preferred server index
  folder["PreferredServerIndex"] << m_ServerURLModel->GetValue();
}

bool DistributedSegmentationModel::AreAllRequiredTagsAssignedTarget()
{
  for(int i = 0; i < m_TagSpecArray.size(); i++)
    {
    if(m_TagSpecArray[i].tag_spec.required && m_TagSpecArray[i].object_id == 0)
      return false;
    }
  return true;
}

bool DistributedSegmentationModel::CheckState(DistributedSegmentationModel::UIState state)
{
  switch(state)
    {
    case DistributedSegmentationModel::UIF_AUTHENTICATED:
      return m_ServerStatusModel->GetValue() == CONNECTED_AUTHORIZED;
    case DistributedSegmentationModel::UIF_TAGS_ASSIGNED:
      return AreAllRequiredTagsAssignedTarget();
    }

  return false;
}

std::vector<std::string> DistributedSegmentationModel::GetUserServerList() const
{
  std::vector<std::string> user_servers;
  user_servers.insert(user_servers.end(),
                      m_ServerURLList.begin() + m_SystemServerURLList.size(),
                      m_ServerURLList.end());
  return user_servers;
}

void DistributedSegmentationModel::SetUserServerList(const std::vector<std::string> &servers)
{
  // Get the current server (the URL model is always valid)
  std::string my_server = m_ServerURLList[this->GetServerURL()];

  // Reset the list of servers
  m_ServerURLList = m_SystemServerURLList;
  m_ServerURLList.insert(m_ServerURLList.end(), servers.begin(), servers.end());

  // Is the selected server still on the list
  std::vector<std::string>::const_iterator it =
      std::find(m_ServerURLList.begin(), m_ServerURLList.end(), my_server);
  if(it == m_ServerURLList.end())
    this->SetServerURL(0);
  else
    this->SetServerURL(it - m_ServerURLList.begin());

  // Update the domain
  m_ServerURLModel->InvokeEvent(DomainChangedEvent());

}

std::string DistributedSegmentationModel::GetURL(const std::string &path)
{
  // Get the main part of the URL
  std::string server = m_ServerURLList[m_ServerURLModel->GetValue()];
  return path.length() ? server + "/" + path : server;
}

void DistributedSegmentationModel::SetServiceListing(const ServiceListing &listing)
{
  // Get the current service info
  int curr_service_id;
  bool is_curr_service_valid = m_CurrentServiceModel->GetValueAndDomain(curr_service_id, NULL);
  std::string curr_service_hash;
  if(is_curr_service_valid)
    curr_service_hash = m_ServiceListing[curr_service_id].githash;

  // Set the service listing
  m_ServiceListing = listing;

  // Deal with empty listing
  if(m_ServiceListing.size() == 0)
    {
    m_CurrentServiceModel->SetDomain(CurrentServiceDomain());
    m_CurrentServiceModel->SetIsValid(false);
    return;
    }

  // Sort the service listing
  std::sort(m_ServiceListing.begin(), m_ServiceListing.end(), dss_model::service_summary_cmp);

  // Generate the domain for the selected service model
  CurrentServiceDomain domain;
  int new_service_id = 0;
  for(int i = 0; i < m_ServiceListing.size(); i++)
    {
    std::ostringstream oss;
    oss << m_ServiceListing[i].name << " " << m_ServiceListing[i].version
        << " : " << m_ServiceListing[i].desc;
    domain[i] = oss.str();

    if(is_curr_service_valid && m_ServiceListing[i].githash == curr_service_hash)
      new_service_id = i;
    }

  // Set the current service
  m_CurrentServiceModel->SetIsValid(true);
  m_CurrentServiceModel->SetDomain(domain);
  m_CurrentServiceModel->SetValue(new_service_id);
}

DistributedSegmentationModel::LoadAction
DistributedSegmentationModel::GetTagLoadAction(int tag_index) const
{
  if(tag_index < 0 || tag_index >= m_TagSpecArray.size())
    return LOAD_NONE;

  TagType type = m_TagSpecArray[tag_index].tag_spec.type;

  bool have_main = m_Parent->GetDriver()->IsMainImageLoaded();
  if(type == TAG_LAYER_MAIN || (type == TAG_LAYER_ANATOMICAL && !have_main))
    {
    return LOAD_MAIN;
    }
  else if((type == TAG_LAYER_OVERLAY || type == TAG_LAYER_ANATOMICAL) && have_main)
    {
    return LOAD_OVERLAY;
    }
  else return LOAD_NONE;
}

std::string DistributedSegmentationModel::GetCurrentServiceGitHash() const
{
  int index;
  if(m_CurrentServiceModel->GetValueAndDomain(index, NULL))
    return m_ServiceListing[index].githash;
  else
    return std::string();
}

void DistributedSegmentationModel::ApplyTagsToTargets()
{
  GenericImageData *id = m_Parent->GetDriver()->GetIRISImageData();
  for(int i = 0; i < m_TagSpecArray.size(); i++)
    {
    TagSpec &ts = m_TagSpecArray[i].tag_spec;
    if(ts.type == TAG_LAYER_MAIN || ts.type == TAG_LAYER_ANATOMICAL || ts.type == TAG_LAYER_OVERLAY)
      {
      ImageWrapperBase *wrapper = id->FindLayer(m_TagSpecArray[i].object_id, false);
      if(wrapper)
        {
        std::list<std::string> tags = wrapper->GetTags();
        if(std::find(tags.begin(), tags.end(), ts.name) == tags.end())
          {
          tags.push_back(ts.name);
          wrapper->SetTags(tags);
          }
        for(LayerIterator it = id->GetLayers(); !it.IsAtEnd(); ++it)
          if(it.GetLayer() != wrapper)
            {
            std::list<std::string> tags = it.GetLayer()->GetTags();
            if(std::find(tags.begin(), tags.end(), ts.name) != tags.end())
              {
              tags.remove(ts.name);
              it.GetLayer()->SetTags(tags);
              }
            }
        }
      }
    }
}

#include "AllPurposeProgressAccumulator.h"


void DistributedSegmentationModel::SubmitWorkspace(ProgressReporterDelegate *pdel)
{
  // At this point the project had to be saved. We read it using the API object
  WorkspaceAPI ws;
  ws.ReadFromXMLFile(m_Parent->GetGlobalState()->GetProjectFilename().c_str());

  // Create a command that reports accumulated progress
  SmartPtr<itk::Command> cmd = pdel->CreateCommand();

  // Do the upload magic
  int ticket_id = ws.CreateWorkspaceTicket(this->GetCurrentServiceGitHash().c_str(), cmd);

  // Stick into the model
  m_SubmittedTicketIdModel->SetValue(ticket_id);
  m_SubmittedTicketIdModel->SetIsValid(true);
}

std::string DistributedSegmentationModel::DownloadWorkspace()
{
  // Is there a valid ticket id with status of success?
  IdType selected_ticket_id;
  if(!m_TicketListModel->GetValueAndDomain(selected_ticket_id, NULL)
     || m_TicketListing.find(selected_ticket_id) == m_TicketListing.end()
     || m_TicketListing[selected_ticket_id].status != STATUS_SUCCESS)
    return "";

  // Create temporary directory for the download (for now)
  string tempdir = WorkspaceAPI::GetTempDirName();
  itksys::SystemTools::MakeDirectory(tempdir);

  // Download into this directory
  std::string file_list =
      WorkspaceAPI::DownloadTicketFiles(selected_ticket_id, tempdir.c_str(), false, "results");

  // return the file list
  return file_list;

}

void DistributedSegmentationModel::DeleteSelectedTicket()
{
  // Is there a valid ticket id with status of success?
  IdType selected_ticket_id;
  if(!m_TicketListModel->GetValueAndDomain(selected_ticket_id, NULL)
     || m_TicketListing.find(selected_ticket_id) == m_TicketListing.end())
    return;

  // Delete the ticket
  RESTClient rc;
  if(!rc.Get("api/tickets/%d/delete", selected_ticket_id))
    throw IRISException("Error deleting ticket %d: %s", selected_ticket_id, rc.GetResponseText());

  // Select the next ticket in the list
  TicketListingResponse::const_iterator it = m_TicketListing.find(selected_ticket_id);
  if(++it != m_TicketListing.end())
    m_TicketListModel->SetValue(it->first);
  else if(m_TicketListing.size())
    m_TicketListModel->SetValue(m_TicketListing.rbegin()->first);
  else
    m_TicketListModel->SetIsValid(false);

  // Remove the ticket
  m_TicketListing.erase(selected_ticket_id);
  m_TicketListModel->InvokeEvent(DomainChangedEvent());
}

IdType DistributedSegmentationModel::GetLastLogIdOfSelectedTicket()
{
  // There must be a selected ticket, the detail must be for that ticket and there
  // must be some log messages in the detail
  IdType selected_ticket_id;
  if(!m_TicketListModel->GetValueAndDomain(selected_ticket_id, NULL)
     || selected_ticket_id != m_SelectedTicketDetail.ticket_id
     || m_SelectedTicketDetail.log.size() == 0)
    {
    return 0;
    }

  // Get the latest id
  return m_SelectedTicketDetail.log.back().id;
}

const TicketDetailResponse *DistributedSegmentationModel::GetSelectedTicketDetail()
{
  // There must be a selected ticket, the detail must be for that ticket and there
  // must be some log messages in the detail
  IdType selected_ticket_id;
  if(!m_TicketListModel->GetValueAndDomain(selected_ticket_id, NULL)
     || selected_ticket_id != m_SelectedTicketDetail.ticket_id)
    {
    return NULL;
    }

  return &m_SelectedTicketDetail;
}

bool
DistributedSegmentationModel::AsyncGetServiceListing(
    std::vector<dss_model::ServiceSummary> &services)
{
  try
  {
  // Second, try to get service listing
  RESTClient rc;

  if(rc.Get("api/services?format=json"))
    {
    Json::Reader json_reader; Json::Value root;
    if(json_reader.parse(rc.GetOutput(), root, false))
      {
      const Json::Value res = root["result"];
      for(int i = 0; i < res.size(); i++)
        {
        dss_model::ServiceSummary service;
        service.name = res[i].get("name","").asString();
        service.githash = res[i].get("githash","").asString();
        service.version = res[i].get("version","").asString();
        service.desc = res[i].get("shortdesc","").asString();
        services.push_back(service);
        }
      }

    return true;
    }
  }
  catch(...)
  {
  }

  return false;
}

StatusCheckResponse
DistributedSegmentationModel::AsyncCheckStatus(std::string url, std::string token)
{
  dss_model::StatusCheckResponse response;
  response.auth_response.connected = false;
  response.auth_response.authenticated = false;

  try
    {
    // If token is empty, bypass the authentication step
    if(token.size() == 0)
      {
      // We
      RESTClient rc;
      rc.SetServerURL(url.c_str());
      }
    else
      {
      RESTClient rc;
      if(!rc.Authenticate(url.c_str(), token.c_str()))
        {
        // Failed authentication but did connect
        response.auth_response.connected = true;
        return response;
        }
      }

    // See if we can successfully get a listing
    if(AsyncGetServiceListing(response.service_listing))
      {
      response.auth_response.connected = true;
      response.auth_response.authenticated = true;
      }
    }
  catch(...)
    {
    response.auth_response.connected = false;
    }

  return response;
}

void DistributedSegmentationModel::ApplyStatusCheckResponse(const StatusCheckResponse &result)
{
  if(result.auth_response.connected)
    {
    if(result.auth_response.authenticated)
      {
      // We no longer need a token
      SetServerStatus(DistributedSegmentationModel::CONNECTED_AUTHORIZED);
      SetToken("");
      }
    else
      SetServerStatus(DistributedSegmentationModel::CONNECTED_NOT_AUTHORIZED);
    }
  else
    {
    SetServerStatus(DistributedSegmentationModel::NOT_CONNECTED);
    }

  SetServiceListing(result.service_listing);
}

ServiceDetailResponse
DistributedSegmentationModel::AsyncGetServiceDetails(std::string githash)
{
  ServiceDetailResponse result;
  result.valid = false;

  RegistryEnumMap<TagType> type_map;
  type_map.AddPair(TAG_POINT_LANDMARK, "PointLandmark");
  type_map.AddPair(TAG_LAYER_MAIN, "MainImage");
  type_map.AddPair(TAG_LAYER_OVERLAY, "OverlayImage");
  type_map.AddPair(TAG_LAYER_ANATOMICAL, "AnatomicalImage");
  type_map.AddPair(TAG_SEGMENTATION_LABEL, "SegmentationLabel");
  type_map.AddPair(TAG_UNKNOWN, "Unknown");

  try {
    RESTClient rc;
    if(rc.Get("api/services/%s/detail", githash.c_str()))
      {
      Json::Reader json_reader;
      Json::Value root;
      if(json_reader.parse(rc.GetOutput(), root, false))
        {
        result.longdesc = root.get("longdesc","").asString();
        result.url = root.get("url","").asString();
        result.valid = true;
        const Json::Value tag_group = root["tags"];
        for(int i = 0; i < tag_group.size(); i++)
          {
          TagSpec tag_spec;
          tag_spec.required = tag_group[i].get("required", false).asBool();
          tag_spec.type = type_map.GetEnumValueWithDefault(
                            tag_group[i].get("type","").asString(), TAG_UNKNOWN);
          tag_spec.name = tag_group[i].get("name","").asString();
          tag_spec.hint = tag_group[i].get("hint","").asString();
          result.tag_specs.push_back(tag_spec);
          }
        }
      }
    }
  catch (...) {

    }

  return result;
}

void DistributedSegmentationModel::AssignTagObjectIds()
{
  // Get the driver
  IRISApplication *driver = this->GetParent()->GetDriver();

  // Handle the main image assignment
  for(int i = 0; i < m_TagSpecArray.size(); i++)
    {
    TagTargetSpec &tag = m_TagSpecArray[i];
    tag.object_id = 0;
    tag.desc = "Unassigned";

    if(driver->IsMainImageLoaded())
      {
      int role_filter = -1;
      switch(tag.tag_spec.type)
        {
        case TAG_LAYER_MAIN: role_filter = MAIN_ROLE; break;
        case TAG_LAYER_OVERLAY: role_filter = OVERLAY_ROLE; break;
        case TAG_LAYER_ANATOMICAL: role_filter = MAIN_ROLE | OVERLAY_ROLE; break;
        default: break;
        }

      if(role_filter >= 0)
        {
        std::list<ImageWrapperBase*> matches =
            driver->GetIRISImageData()->FindLayersByTag(tag.tag_spec.name, role_filter);
        if(matches.size() == 1)
          {
          tag.object_id = matches.front()->GetUniqueId();
          tag.desc = matches.front()->GetNickname();
          }
        }
      }
    }
}

void DistributedSegmentationModel::ApplyServiceDetailResponse(const ServiceDetailResponse &resp)
{
  this->SetServiceDescription(resp.longdesc);

  // Store the tag spec array
  m_TagSpecArray.clear();
  for(int i = 0; i < resp.tag_specs.size(); i++)
    {
    TagTargetSpec ttspec;
    ttspec.tag_spec = resp.tag_specs[i];
    ttspec.object_id = 0;
    m_TagSpecArray.push_back(ttspec);
    }

  // Assign tag ids to objects in current workspace
  this->AssignTagObjectIds();

  // Fire off a domain modified event
  m_TagListModel->InvokeEvent(DomainChangedEvent());
  if(resp.tag_specs.size())
    {
    m_TagListModel->SetValue(0);
    m_TagListModel->SetIsValid(true);
    }
  else
    {
    m_TagListModel->SetIsValid(false);
    }
}

TicketListingResponse DistributedSegmentationModel::AsyncGetTicketListing()
{
  TicketListingResponse result;

  try {
    RESTClient rc;
    if(rc.Get("api/tickets?format=json"))
      {
      Json::Reader json_reader;
      Json::Value root;
      if(json_reader.parse(rc.GetOutput(), root, false))
        {
        const Json::Value tickets = root["result"];
        for(int i = 0; i < tickets.size(); i++)
          {
          TicketStatusSummary tss;
          tss.id = (IdType) tickets[i].get("id",0).asLargestInt();
          tss.service_name = tickets[i].get("service","").asString();
          tss.status = ticket_status_emap.GetEnumValueWithDefault(
                         tickets[i].get("status","").asString(), STATUS_UNKNOWN);
          result[tss.id] = tss;
          }
        }
      }
    }
  catch (...)
  {
  }

  return result;
}

void DistributedSegmentationModel::ApplyTicketListingResponse(const TicketListingResponse &resp)
{
  // Check if the ticket listing has changed
  bool same_keys = true;
  if(m_TicketListing.size() != resp.size())
    {
    same_keys = false;
    }
  else
    {
    TicketListingResponse::const_iterator it_old = m_TicketListing.begin();
    TicketListingResponse::const_iterator it_new = resp.begin();

    for(; it_old != m_TicketListing.end() && it_new != resp.end(); it_old++, it_new++)
      {
      if(it_old->first != it_new->first)
        {
        same_keys = false;
        break;
        }
      }
    }

  // Just store the ticket listing
  m_TicketListing = resp;

  // Set the status of the model
  m_TicketListModel->SetIsValid(m_TicketListing.size() > 0);

  if(same_keys)
    m_TicketListModel->InvokeEvent(DomainDescriptionChangedEvent());
  else
    m_TicketListModel->InvokeEvent(DomainChangedEvent());
}

TicketDetailResponse DistributedSegmentationModel::AsyncGetTicketDetails(IdType ticket_id, IdType last_log)
{
  TicketDetailResponse tdr;
  tdr.ticket_id = ticket_id;
  tdr.progress = 0.0;

  try {

    // Get a full update on this ticket
    RESTClient rc;
    if(rc.Get("api/tickets/%ld/detail?since=%ld", ticket_id, last_log))
      {
      Json::Reader json_reader;
      Json::Value root;
      if(json_reader.parse(rc.GetOutput(), root, false))
        {
        const Json::Value result = root["result"];

        // Read progress
        tdr.progress = result.get("progress",0.0).asDouble();

        // Read logs
        const Json::Value log_entry = result["log"];
        for(int i = 0; i < log_entry.size(); i++)
          {
          TicketLogEntry loglet;
          loglet.id = log_entry[i].get("id",0).asLargestInt();
          loglet.type = log_type_emap.GetEnumValueWithDefault(
                             log_entry[i].get("category","").asString(), LOG_UNKNOWN);
          loglet.atime = log_entry[i].get("atime","").asString();
          loglet.text = log_entry[i].get("message","").asString();

          const Json::Value att_entry = log_entry[i]["attachments"];
          for(int i = 0; i < att_entry.size(); i++)
            {
            Attachment att;
            att.desc = att_entry[i].get("description","").asString();
            att.url = att_entry[i].get("url","").asString();
            att.mimetype = att_entry[i].get("mime_type","").asString();
            loglet.attachments.push_back(att);
            }

          tdr.log.push_back(loglet);
          }
        }
      }
    }
  catch (...) {

    }

  return tdr;
}

void DistributedSegmentationModel::ApplyTicketDetailResponse(const TicketDetailResponse &resp)
{
  // Make sure that the detail is for the ticket that is currently selected
  IdType selected_ticket_id;
  if(!m_TicketListModel->GetValueAndDomain(selected_ticket_id, NULL)
     || selected_ticket_id != resp.ticket_id)
    {
    // Just ignore this update - it is irrelevant because we selected another ticket already
    return;
    }

  // Store the progress
  m_SelectedTicketProgressModel->SetValue(resp.progress);
  m_SelectedTicketProgressModel->SetIsValid(true);

  // Store the log for the current ticket
  bool log_modified = false;
  if(m_SelectedTicketDetail.ticket_id != resp.ticket_id)
    {
    m_SelectedTicketDetail.log.clear();
    log_modified = true;
    }

  // Append the log
  for(std::vector<TicketLogEntry>::const_iterator it = resp.log.begin(); it != resp.log.end(); ++it)
    {
    m_SelectedTicketDetail.log.push_back(*it);
    log_modified = true;
    }

  // Update the other fields
  m_SelectedTicketDetail.progress = resp.progress;
  m_SelectedTicketDetail.ticket_id = resp.ticket_id;

  // Cause update in the log model
  m_SelectedTicketLogModel->SetIsValid(true);
  if(log_modified)
    m_SelectedTicketLogModel->InvokeEvent(DomainChangedEvent());
}

bool DistributedSegmentationModel::GetServerStatusStringValue(std::string &value)
{
  ServerStatus status;
  ServerStatusDomain domain;
  if(m_ServerStatusModel->GetValueAndDomain(status, &domain))
    {
    value = domain[status];
    return true;
    }
  return false;
}

bool DistributedSegmentationModel
::GetCurrentTagImageLayerValueAndRange(unsigned long &value, LayerSelectionDomain *domain)
{
  int curr_tag;
  if(m_TagListModel->GetValueAndDomain(curr_tag, NULL))
    {
    TagTargetSpec &tag = m_TagSpecArray[curr_tag];
    value = tag.object_id;
    if(domain)
      {
      domain->clear();
      (*domain)[0] = "Unassigned";
      IRISApplication *driver = this->GetParent()->GetDriver();

      int role_filter = -1;
      switch(tag.tag_spec.type)
        {
        case TAG_LAYER_MAIN: role_filter = MAIN_ROLE; break;
        case TAG_LAYER_OVERLAY: role_filter = OVERLAY_ROLE; break;
        case TAG_LAYER_ANATOMICAL: role_filter = MAIN_ROLE | OVERLAY_ROLE; break;
        default: break;
        }
      if(role_filter >= 0 && driver->IsMainImageLoaded())
        {
        for(LayerIterator it = driver->GetIRISImageData()->GetLayers(role_filter);
            !it.IsAtEnd(); ++it)
          {
          (*domain)[it.GetLayer()->GetUniqueId()] = it.GetLayer()->GetNickname();
          }
        }
      }
    return true;
    }
  return false;
}

void DistributedSegmentationModel
::SetCurrentTagImageLayerValue(unsigned long value)
{
  int curr_tag;
  IRISApplication *driver = this->GetParent()->GetDriver();
  if(m_TagListModel->GetValueAndDomain(curr_tag, NULL))
    {
    // Set the target id
    m_TagSpecArray[curr_tag].object_id = value;

    // Set the target description
    ImageWrapperBase *w = driver->GetIRISImageData()->FindLayer(value, false);
    m_TagSpecArray[curr_tag].desc = w ? w->GetNickname() : "Unassigned";

    // Update the domain
    m_TagListModel->InvokeEvent(DomainChangedEvent());
    }
}

DistributedSegmentationModel::DistributedSegmentationModel()
{
  // Build a list of available URLs
  m_SystemServerURLList.push_back("https://dss.itksnap.org");

  // Add system URLs to the url list
  m_ServerURLList = m_SystemServerURLList;

  // Create the server model that references the URL list
  m_ServerURLModel = NewConcreteProperty(0, ServerURLDomain(&m_ServerURLList));

  // Create the token model
  m_TokenModel = NewSimpleConcreteProperty(std::string(""));

  // Server status model
  ServerStatusDomain server_status_dom;
  server_status_dom[NOT_CONNECTED] = "Not connected";
  server_status_dom[CONNECTED_NOT_AUTHORIZED] = "Connected, Not Authorized";
  server_status_dom[CONNECTED_AUTHORIZED] = "Connected and Authorized";
  m_ServerStatusModel = NewConcreteProperty(NOT_CONNECTED, server_status_dom);

  // Server status string
  m_ServerStatusStringModel
      = wrapGetterSetterPairAsProperty(this, &Self::GetServerStatusStringValue);
  m_ServerStatusStringModel->RebroadcastFromSourceProperty(m_ServerStatusModel);

  // Initialize the service model
  m_CurrentServiceModel = NewConcreteProperty(-1, CurrentServiceDomain());
  m_CurrentServiceModel->SetIsValid(false);

  // Service description
  m_ServiceDescriptionModel = NewSimpleConcreteProperty(std::string());

  // Tag selection model
  m_TagListModel = NewConcreteProperty(-1, TagDomainType(&m_TagSpecArray));
  m_TagListModel->SetIsValid(false);

  // Ticket listing model
  m_TicketListModel = NewConcreteProperty((IdType) -1, TicketListingDomain(&m_TicketListing));
  m_TicketListModel->SetIsValid(false);

  // Model for current tag selection
  m_CurrentTagImageLayerModel = wrapGetterSetterPairAsProperty(
                                  this,
                                  &Self::GetCurrentTagImageLayerValueAndRange,
                                  &Self::SetCurrentTagImageLayerValue);
  m_CurrentTagImageLayerModel->Rebroadcast(m_TagListModel, ValueChangedEvent(), ValueChangedEvent());
  m_CurrentTagImageLayerModel->Rebroadcast(m_TagListModel, ValueChangedEvent(), DomainChangedEvent());

  // Last submitted ticket
  m_SubmittedTicketIdModel = NewSimpleConcreteProperty(-1);
  m_SubmittedTicketIdModel->SetIsValid(false);

  // Selected ticket progress model
  m_SelectedTicketProgressModel = NewRangedConcreteProperty(0.0, 0.0, 1.0, 0.01);
  m_SelectedTicketProgressModel->SetIsValid(false);

  // Selected ticket logs
  m_SelectedTicketLogModel = NewConcreteProperty((IdType) -1, LogDomainType(&m_SelectedTicketDetail.log));
  m_SelectedTicketLogModel->SetIsValid(false);

  // Changes to the server and token result in a server change event
  this->Rebroadcast(m_ServerURLModel, ValueChangedEvent(), ServerChangeEvent());
  this->Rebroadcast(m_ServerURLModel, DomainChangedEvent(), ServerChangeEvent());
  this->Rebroadcast(m_TokenModel, ValueChangedEvent(), ServerChangeEvent());

  // Changes to the selected service also propagated
  this->Rebroadcast(m_CurrentServiceModel, ValueChangedEvent(), ServiceChangeEvent());
  this->Rebroadcast(m_CurrentServiceModel, DomainChangedEvent(), ServiceChangeEvent());

  // Changes to the tags table require a state update
  this->Rebroadcast(m_CurrentTagImageLayerModel, DomainChangedEvent(), StateMachineChangeEvent());
  this->Rebroadcast(m_ServerStatusModel, ValueChangedEvent(), StateMachineChangeEvent());
  this->Rebroadcast(m_TagListModel, DomainChangedEvent(), StateMachineChangeEvent());

}


