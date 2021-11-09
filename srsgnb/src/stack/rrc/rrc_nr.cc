/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2021 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "srsgnb/hdr/stack/rrc/rrc_nr.h"
#include "srsenb/hdr/common/common_enb.h"
#include "srsgnb/hdr/stack/rrc/cell_asn1_config.h"
#include "srsgnb/src/stack/mac/test/sched_nr_cfg_generators.h"
#include "srsran/asn1/rrc_nr_utils.h"
#include "srsran/common/common_nr.h"
#include "srsran/common/phy_cfg_nr_default.h"
#include "srsran/common/standard_streams.h"
#include "srsran/common/string_helpers.h"

using namespace asn1::rrc_nr;

namespace srsenb {

rrc_nr::rrc_nr(srsran::task_sched_handle task_sched_) :
  logger(srslog::fetch_basic_logger("RRC-NR")), task_sched(task_sched_)
{}

int rrc_nr::init(const rrc_nr_cfg_t&         cfg_,
                 phy_interface_stack_nr*     phy_,
                 mac_interface_rrc_nr*       mac_,
                 rlc_interface_rrc*          rlc_,
                 pdcp_interface_rrc*         pdcp_,
                 ngap_interface_rrc_nr*      ngap_,
                 gtpu_interface_rrc_nr*      gtpu_,
                 rrc_eutra_interface_rrc_nr* rrc_eutra_)
{
  phy       = phy_;
  mac       = mac_;
  rlc       = rlc_;
  pdcp      = pdcp_;
  ngap      = ngap_;
  gtpu      = gtpu_;
  rrc_eutra = rrc_eutra_;

  cfg = cfg_;
  if (cfg.is_standalone) {
    // Generate parameters of Coreset#0 and SS#0
    const uint32_t coreset0_idx                        = 7;
    cfg.cell_list[0].phy_cell.pdcch.coreset_present[0] = true;
    // Get pointA and SSB absolute frequencies
    double pointA_abs_freq_Hz = cfg.cell_list[0].phy_cell.carrier.dl_center_frequency_hz -
                                cfg.cell_list[0].phy_cell.carrier.nof_prb * SRSRAN_NRE *
                                    SRSRAN_SUBC_SPACING_NR(cfg.cell_list[0].phy_cell.carrier.scs) / 2;
    double ssb_abs_freq_Hz = cfg.cell_list[0].phy_cell.carrier.ssb_center_freq_hz;
    // Calculate integer SSB to pointA frequency offset in Hz
    uint32_t ssb_pointA_freq_offset_Hz =
        (ssb_abs_freq_Hz > pointA_abs_freq_Hz) ? (uint32_t)(ssb_abs_freq_Hz - pointA_abs_freq_Hz) : 0;
    int ret = srsran_coreset_zero(cfg.cell_list[0].phy_cell.cell_id,
                                  ssb_pointA_freq_offset_Hz,
                                  cfg.cell_list[0].ssb_cfg.scs,
                                  cfg.cell_list[0].phy_cell.carrier.scs,
                                  coreset0_idx,
                                  &cfg.cell_list[0].phy_cell.pdcch.coreset[0]);
    srsran_assert(ret == SRSRAN_SUCCESS, "Failed to generate CORESET#0");
    cfg.cell_list[0].phy_cell.pdcch.search_space_present[0]           = true;
    cfg.cell_list[0].phy_cell.pdcch.search_space[0].id                = 0;
    cfg.cell_list[0].phy_cell.pdcch.search_space[0].coreset_id        = 0;
    cfg.cell_list[0].phy_cell.pdcch.search_space[0].type              = srsran_search_space_type_common_0;
    cfg.cell_list[0].phy_cell.pdcch.search_space[0].nof_candidates[0] = 1;
    cfg.cell_list[0].phy_cell.pdcch.search_space[0].nof_candidates[1] = 1;
    cfg.cell_list[0].phy_cell.pdcch.search_space[0].nof_candidates[2] = 1;
    cfg.cell_list[0].phy_cell.pdcch.search_space[0].formats[0]        = srsran_dci_format_nr_1_0;
    cfg.cell_list[0].phy_cell.pdcch.search_space[0].nof_formats       = 1;
    cfg.cell_list[0].phy_cell.pdcch.search_space[0].duration          = 1;
  }

  cell_ctxt.reset(new cell_ctxt_t{});

  // derived
  slot_dur_ms = 1;

  if (generate_sibs() != SRSRAN_SUCCESS) {
    logger.error("Couldn't generate SIB messages.");
    return SRSRAN_ERROR;
  }

  // Fill base ASN1 cell config.
  int ret = fill_sp_cell_cfg_from_enb_cfg(cfg, UE_PSCELL_CC_IDX, base_sp_cell_cfg);
  srsran_assert(ret == SRSRAN_SUCCESS, "Failed to configure cell");

  pdcch_cfg_common_s* asn1_pdcch;
  if (not cfg.is_standalone) {
    // Fill rrc_nr_cfg with UE-specific search spaces and coresets
    asn1_pdcch =
        &base_sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.dl_cfg_common.init_dl_bwp.pdcch_cfg_common.setup();
  } else {
    asn1_pdcch = &cell_ctxt->sib1.serving_cell_cfg_common.dl_cfg_common.init_dl_bwp.pdcch_cfg_common.setup();
  }
  bool ret2 = srsran::fill_phy_pdcch_cfg_common(*asn1_pdcch, &cfg.cell_list[0].phy_cell.pdcch);
  srsran_assert(ret2, "Invalid NR cell configuration.");
  ret2 = srsran::fill_phy_pdcch_cfg(base_sp_cell_cfg.sp_cell_cfg_ded.init_dl_bwp.pdcch_cfg.setup(),
                                    &cfg.cell_list[0].phy_cell.pdcch);
  srsran_assert(ret2, "Invalid NR cell configuration.");

  config_phy(); // if PHY is not yet initialized, config will be stored and applied on initialization
  config_mac();

  running = true;

  return SRSRAN_SUCCESS;
}

void rrc_nr::stop()
{
  if (running) {
    running = false;
  }
  users.clear();
}

template <class T>
void rrc_nr::log_rrc_message(const char*             source,
                             const direction_t       dir,
                             srsran::const_byte_span pdu,
                             const T&                msg,
                             const char*             msg_type)
{
  if (logger.debug.enabled()) {
    asn1::json_writer json_writer;
    msg.to_json(json_writer);
    logger.debug(pdu.data(), pdu.size(), "%s - %s %s (%d B)", source, (dir == Rx) ? "Rx" : "Tx", msg_type, pdu.size());
    logger.debug("Content:%s", json_writer.to_string().c_str());
  } else if (logger.info.enabled()) {
    logger.info(pdu.data(), pdu.size(), "%s - %s %s (%d B)", source, (dir == Rx) ? "Rx" : "Tx", msg_type, pdu.size());
  }
}

void rrc_nr::log_rx_pdu_fail(uint16_t                rnti,
                             uint32_t                lcid,
                             srsran::const_byte_span pdu,
                             const char*             cause_str,
                             bool                    log_hex)
{
  if (log_hex) {
    logger.error(
        pdu.data(), pdu.size(), "Rx %s PDU, rnti=0x%x - Discarding. Cause: %s", get_rb_name(lcid), rnti, cause_str);
  } else {
    logger.error("Rx %s PDU, rnti=0x%x - Discarding. Cause: %s", get_rb_name(lcid), rnti, cause_str);
  }
}

/* @brief PRIVATE function, gets called by sgnb_addition_request
 *
 * This function WILL NOT TRIGGER the RX MSG3 activity timer
 */
int rrc_nr::add_user(uint16_t rnti, const sched_nr_ue_cfg_t& uecfg, bool start_msg3_timer)
{
  if (users.contains(rnti) == 0) {
    // If in the ue ctor, "start_msg3_timer" is set to true, this will start the MSG3 RX TIMEOUT at ue creation
    users.insert(rnti, std::unique_ptr<ue>(new ue(this, rnti, uecfg, start_msg3_timer)));
    rlc->add_user(rnti);
    pdcp->add_user(rnti);
    logger.info("Added new user rnti=0x%x", rnti);
    return SRSRAN_SUCCESS;
  } else {
    logger.error("Adding user rnti=0x%x (already exists)", rnti);
    return SRSRAN_ERROR;
  }
}

/* @brief PUBLIC function, gets called by mac_nr::rach_detected
 *
 * This function is called from PRACH worker (can wait) and WILL TRIGGER the RX MSG3 activity timer
 */
int rrc_nr::add_user(uint16_t rnti, const sched_nr_ue_cfg_t& uecfg)
{
  // Set "triggered_by_rach" to true to start the MSG3 RX TIMEOUT
  return add_user(rnti, uecfg, true);
}

void rrc_nr::rem_user(uint16_t rnti)
{
  auto user_it = users.find(rnti);
  if (user_it != users.end()) {
    // First remove MAC and GTPU to stop processing DL/UL traffic for this user
    mac->remove_ue(rnti); // MAC handles PHY
    rlc->rem_user(rnti);
    pdcp->rem_user(rnti);
    users.erase(rnti);

    srsran::console("Disconnecting rnti=0x%x.\n", rnti);
    logger.info("Removed user rnti=0x%x", rnti);
  } else {
    logger.error("Removing user rnti=0x%x (does not exist)", rnti);
  }
}

/* Function called by MAC after the reception of a C-RNTI CE indicating that the UE still has a
 * valid RNTI.
 */
int rrc_nr::update_user(uint16_t new_rnti, uint16_t old_rnti)
{
  if (new_rnti == old_rnti) {
    logger.warning("rnti=0x%x received MAC CRNTI CE with same rnti", new_rnti);
    return SRSRAN_ERROR;
  }

  // Remove new_rnti
  auto new_ue_it = users.find(new_rnti);
  if (new_ue_it != users.end()) {
    new_ue_it->second->deactivate_bearers();
    task_sched.defer_task([this, new_rnti]() { rem_user(new_rnti); });
  }

  // Send Reconfiguration to old_rnti if is RRC_CONNECT or RRC Release if already released here
  auto old_it = users.find(old_rnti);
  if (old_it == users.end()) {
    logger.info("rnti=0x%x received MAC CRNTI CE: 0x%x, but old context is unavailable", new_rnti, old_rnti);
    return SRSRAN_ERROR;
  }
  ue* ue_ptr = old_it->second.get();

  logger.info("Resuming rnti=0x%x RRC connection due to received C-RNTI CE from rnti=0x%x.", old_rnti, new_rnti);
  ue_ptr->crnti_ce_received();

  return SRSRAN_SUCCESS;
}

void rrc_nr::set_activity_user(uint16_t rnti)
{
  auto it = users.find(rnti);
  if (it == users.end()) {
    logger.info("rnti=0x%x not found. Can't set activity", rnti);
    return;
  }
  ue* ue_ptr = it->second.get();

  // inform EUTRA RRC about user activity
  if (ue_ptr->is_endc()) {
    // Restart inactivity timer for RRC-NR
    ue_ptr->set_activity();
    // inform EUTRA RRC about user activity
    rrc_eutra->set_activity_user(ue_ptr->get_eutra_rnti());
  }
}

void rrc_nr::config_phy()
{
  srsenb::phy_interface_rrc_nr::common_cfg_t common_cfg = {};
  common_cfg.carrier                                    = cfg.cell_list[0].phy_cell.carrier;
  common_cfg.pdcch                                      = cfg.cell_list[0].phy_cell.pdcch;
  common_cfg.prach                                      = cfg.cell_list[0].phy_cell.prach;
  common_cfg.duplex_mode                                = cfg.cell_list[0].duplex_mode;
  common_cfg.ssb                                        = cfg.cell_list[0].ssb_cfg;
  if (phy->set_common_cfg(common_cfg) < SRSRAN_SUCCESS) {
    logger.error("Couldn't set common PHY config");
    return;
  }
}

void rrc_nr::config_mac()
{
  // Fill MAC scheduler configuration for SIBs
  // TODO: use parsed cell NR cfg configuration
  std::vector<srsenb::sched_nr_interface::cell_cfg_t> sched_cells_cfg = {srsenb::get_default_cells_cfg(1)};
  sched_nr_interface::cell_cfg_t&                     cell            = sched_cells_cfg[0];

  // Derive cell config from rrc_nr_cfg_t
  cell.bwps[0].pdcch = cfg.cell_list[0].phy_cell.pdcch;
  // Derive cell config from ASN1
  bool ret2 = srsran::make_pdsch_cfg_from_serv_cell(base_sp_cell_cfg.sp_cell_cfg_ded, &cell.bwps[0].pdsch);
  srsran_assert(ret2, "Invalid NR cell configuration.");
  ret2 = srsran::make_phy_ssb_cfg(
      cfg.cell_list[0].phy_cell.carrier, base_sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common, &cell.ssb);
  srsran_assert(ret2, "Invalid NR cell configuration.");
  ret2 = srsran::make_duplex_cfg_from_serv_cell(base_sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common, &cell.duplex);
  srsran_assert(ret2, "Invalid NR cell configuration.");

  // Set SIB1 and SI messages
  cell.sibs.resize(cell_ctxt->sib_buffer.size());
  for (uint32_t i = 0; i < cell_ctxt->sib_buffer.size(); i++) {
    cell.sibs[i].len = cell_ctxt->sib_buffer[i]->N_bytes;
    if (i == 0) {
      cell.sibs[i].period_rf       = 16; // SIB1 is always 16 rf
      cell.sibs[i].si_window_slots = 160;
    } else {
      cell.sibs[i].period_rf       = cell_ctxt->sib1.si_sched_info.sched_info_list[i - 1].si_periodicity.to_number();
      cell.sibs[i].si_window_slots = cell_ctxt->sib1.si_sched_info.si_win_len.to_number();
    }
  }

  // Configure MAC/scheduler
  mac->cell_cfg(sched_cells_cfg);
}

int32_t rrc_nr::generate_sibs()
{
  // MIB packing
  fill_mib_from_enb_cfg(cfg, cell_ctxt->mib);
  bcch_bch_msg_s mib_msg;
  mib_msg.msg.set_mib() = cell_ctxt->mib;
  {
    srsran::unique_byte_buffer_t mib_buf = srsran::make_byte_buffer();
    if (mib_buf == nullptr) {
      logger.error("Couldn't allocate PDU in %s().", __FUNCTION__);
      return SRSRAN_ERROR;
    }
    asn1::bit_ref bref(mib_buf->msg, mib_buf->get_tailroom());
    if (mib_msg.pack(bref) != asn1::SRSASN_SUCCESS) {
      logger.error("Couldn't pack mib msg");
      return SRSRAN_ERROR;
    }
    mib_buf->N_bytes = bref.distance_bytes();
    logger.debug(mib_buf->msg, mib_buf->N_bytes, "MIB payload (%d B)", mib_buf->N_bytes);
    cell_ctxt->mib_buffer = std::move(mib_buf);
  }

  if (not cfg.is_standalone) {
    return SRSRAN_SUCCESS;
  }

  // SIB1 packing
  fill_sib1_from_enb_cfg(cfg, cell_ctxt->sib1);
  si_sched_info_s::sched_info_list_l_& sched_info = cell_ctxt->sib1.si_sched_info.sched_info_list;

  // SI messages packing
  cell_ctxt->sibs.resize(1);
  sib2_s& sib2                             = cell_ctxt->sibs[0].set_sib2();
  sib2.cell_resel_info_common.q_hyst.value = asn1::rrc_nr::sib2_s::cell_resel_info_common_s_::q_hyst_opts::db5;

  // msg is array of SI messages, each SI message msg[i] may contain multiple SIBs
  // all SIBs in a SI message msg[i] share the same periodicity
  const uint32_t nof_messages =
      cell_ctxt->sib1.si_sched_info_present ? cell_ctxt->sib1.si_sched_info.sched_info_list.size() : 0;
  cell_ctxt->sib_buffer.reserve(nof_messages + 1);
  asn1::dyn_array<bcch_dl_sch_msg_s> msg(nof_messages + 1);

  // Copy SIB1 to first SI message
  msg[0].msg.set_c1().set_sib_type1() = cell_ctxt->sib1;

  // Copy rest of SIBs
  for (uint32_t sched_info_elem = 0; sched_info_elem < nof_messages; sched_info_elem++) {
    uint32_t msg_index = sched_info_elem + 1; // first msg is SIB1, therefore start with second

    msg[msg_index].msg.set_c1().set_sys_info().crit_exts.set_sys_info();
    auto& sib_list = msg[msg_index].msg.c1().sys_info().crit_exts.sys_info().sib_type_and_info;

    for (uint32_t mapping = 0; mapping < sched_info[sched_info_elem].sib_map_info.size(); ++mapping) {
      uint32_t sibidx = sched_info[sched_info_elem].sib_map_info[mapping].type; // SIB2 == 0
      sib_list.push_back(cell_ctxt->sibs[sibidx]);
    }
  }

  // Pack payload for all messages
  for (uint32_t msg_index = 0; msg_index < nof_messages + 1; msg_index++) {
    srsran::unique_byte_buffer_t sib_pdu = pack_into_pdu(msg[msg_index]);
    if (sib_pdu == nullptr) {
      logger.error("Failed to pack SIB");
      return SRSRAN_ERROR;
    }
    cell_ctxt->sib_buffer.push_back(std::move(sib_pdu));

    // Log SIBs in JSON format
    fmt::memory_buffer strbuf;
    if (msg_index == 0) {
      fmt::format_to(strbuf, "SIB1 payload");
    } else {
      fmt::format_to(strbuf, "SI message={} payload", msg_index + 1);
    }
    log_rrc_message("BCCH", Tx, *cell_ctxt->sib_buffer.back(), msg[msg_index], srsran::to_c_str(strbuf));
  }

  return SRSRAN_SUCCESS;
}

/*******************************************************************************
  MAC interface
*******************************************************************************/

int rrc_nr::read_pdu_bcch_bch(const uint32_t tti, srsran::byte_buffer_t& buffer)
{
  if (cell_ctxt->mib_buffer == nullptr || buffer.get_tailroom() < cell_ctxt->mib_buffer->N_bytes) {
    return SRSRAN_ERROR;
  }
  buffer = *cell_ctxt->mib_buffer;
  return SRSRAN_SUCCESS;
}

int rrc_nr::read_pdu_bcch_dlsch(uint32_t sib_index, srsran::byte_buffer_t& buffer)
{
  if (sib_index >= cell_ctxt->sib_buffer.size()) {
    logger.error("SI%s%d is not a configured SIB.", sib_index == 0 ? "B" : "", sib_index + 1);
    return SRSRAN_ERROR;
  }

  buffer = *cell_ctxt->sib_buffer[sib_index];

  return SRSRAN_SUCCESS;
}

void rrc_nr::get_metrics(srsenb::rrc_metrics_t& m)
{
  if (running) {
    for (auto& ue : users) {
      rrc_ue_metrics_t ue_metrics;
      ue.second->get_metrics(ue_metrics);
      m.ues.push_back(ue_metrics);
    }
  }
}

void rrc_nr::handle_pdu(uint16_t rnti, uint32_t lcid, srsran::const_byte_span pdu)
{
  switch (static_cast<srsran::nr_srb>(lcid)) {
    case srsran::nr_srb::srb0:
      handle_ul_ccch(rnti, pdu);
      break;
    case srsran::nr_srb::srb1:
    case srsran::nr_srb::srb2:
    case srsran::nr_srb::srb3:
      handle_ul_dcch(rnti, lcid, std::move(pdu));
      break;
    default:
      std::string errcause = fmt::format("Invalid LCID=%d", lcid);
      log_rx_pdu_fail(rnti, lcid, pdu, errcause.c_str());
      break;
  }
}

void rrc_nr::handle_ul_ccch(uint16_t rnti, srsran::const_byte_span pdu)
{
  // Parse UL-CCCH
  ul_ccch_msg_s ul_ccch_msg;
  {
    asn1::cbit_ref bref(pdu.data(), pdu.size());
    if (ul_ccch_msg.unpack(bref) != asn1::SRSASN_SUCCESS or
        ul_ccch_msg.msg.type().value != ul_ccch_msg_type_c::types_opts::c1) {
      log_rx_pdu_fail(rnti, srb_to_lcid(lte_srb::srb0), pdu, "Failed to unpack UL-CCCH message", true);
      return;
    }
  }

  // Log Rx message
  fmt::memory_buffer fmtbuf, fmtbuf2;
  fmt::format_to(fmtbuf, "rnti=0x{:x}, SRB0", rnti);
  fmt::format_to(fmtbuf2, "UL-CCCH.{}", ul_ccch_msg.msg.c1().type().to_string());
  log_rrc_message(srsran::to_c_str(fmtbuf), Rx, pdu, ul_ccch_msg, srsran::to_c_str(fmtbuf2));

  // Handle message
  switch (ul_ccch_msg.msg.c1().type().value) {
    case ul_ccch_msg_type_c::c1_c_::types_opts::rrc_setup_request:
      handle_rrc_setup_request(rnti, ul_ccch_msg.msg.c1().rrc_setup_request());
      break;
    default:
      log_rx_pdu_fail(rnti, srb_to_lcid(lte_srb::srb0), pdu, "Unsupported UL-CCCH message type");
      // TODO Remove user
  }
}

void rrc_nr::handle_ul_dcch(uint16_t rnti, uint32_t lcid, srsran::const_byte_span pdu)
{
  // Parse UL-DCCH
  ul_dcch_msg_s ul_dcch_msg;
  {
    asn1::cbit_ref bref(pdu.data(), pdu.size());
    if (ul_dcch_msg.unpack(bref) != asn1::SRSASN_SUCCESS or
        ul_dcch_msg.msg.type().value != ul_dcch_msg_type_c::types_opts::c1) {
      log_rx_pdu_fail(rnti, lcid, pdu, "Failed to unpack UL-DCCH message");
      return;
    }
  }

  // Verify UE exists
  auto ue_it = users.find(rnti);
  if (ue_it == users.end()) {
    log_rx_pdu_fail(rnti, lcid, pdu, "Inexistent rnti");
  }
  ue& u = *ue_it->second;

  // Log Rx message
  fmt::memory_buffer fmtbuf, fmtbuf2;
  fmt::format_to(fmtbuf, "rnti=0x{:x}, {}", rnti, srsran::get_srb_name(srsran::nr_lcid_to_srb(lcid)));
  fmt::format_to(fmtbuf2, "UL-DCCH.{}", ul_dcch_msg.msg.c1().type().to_string());
  log_rrc_message(srsran::to_c_str(fmtbuf), Rx, pdu, ul_dcch_msg, srsran::to_c_str(fmtbuf2));

  // Handle message
  switch (ul_dcch_msg.msg.c1().type().value) {
    case ul_dcch_msg_type_c::c1_c_::types_opts::rrc_setup_complete:
      u.handle_rrc_setup_complete(ul_dcch_msg.msg.c1().rrc_setup_complete());
      break;
    default:
      log_rx_pdu_fail(rnti, srb_to_lcid(lte_srb::srb0), pdu, "Unsupported UL-CCCH message type", false);
      // TODO Remove user
  }
}

void rrc_nr::handle_rrc_setup_request(uint16_t rnti, const asn1::rrc_nr::rrc_setup_request_s& msg)
{
  auto ue_it = users.find(rnti);

  // TODO: Defer creation of ue to this point
  if (ue_it == users.end()) {
    logger.error("%s received for inexistent rnti=0x%x", "UL-CCCH", rnti);
    return;
  }
  ue& u = *ue_it->second;
  u.handle_rrc_setup_request(msg);
}

/*******************************************************************************
  PDCP interface
*******************************************************************************/
void rrc_nr::write_pdu(uint16_t rnti, uint32_t lcid, srsran::unique_byte_buffer_t pdu)
{
  if (pdu == nullptr or pdu->N_bytes == 0) {
    logger.error("Rx %s PDU, rnti=0x%x - Discarding. Cause: PDU is empty", srsenb::get_rb_name(lcid), rnti);
    return;
  }
  handle_pdu(rnti, lcid, *pdu);
}

void rrc_nr::notify_pdcp_integrity_error(uint16_t rnti, uint32_t lcid) {}

/*******************************************************************************
  NGAP interface
*******************************************************************************/

int rrc_nr::ue_set_security_cfg_key(uint16_t rnti, const asn1::fixed_bitstring<256, false, true>& key)
{
  return SRSRAN_SUCCESS;
}
int rrc_nr::ue_set_bitrates(uint16_t rnti, const asn1::ngap_nr::ue_aggregate_maximum_bit_rate_s& rates)
{
  return SRSRAN_SUCCESS;
}
int rrc_nr::set_aggregate_max_bitrate(uint16_t rnti, const asn1::ngap_nr::ue_aggregate_maximum_bit_rate_s& rates)
{
  return SRSRAN_SUCCESS;
}
int rrc_nr::ue_set_security_cfg_capabilities(uint16_t rnti, const asn1::ngap_nr::ue_security_cap_s& caps)
{
  return SRSRAN_SUCCESS;
}
int rrc_nr::start_security_mode_procedure(uint16_t rnti)
{
  return SRSRAN_SUCCESS;
}
int rrc_nr::establish_rrc_bearer(uint16_t rnti, uint16_t pdu_session_id, srsran::const_byte_span nas_pdu, uint32_t lcid)
{
  return SRSRAN_SUCCESS;
}

int rrc_nr::release_bearers(uint16_t rnti)
{
  return SRSRAN_SUCCESS;
}

int rrc_nr::allocate_lcid(uint16_t rnti)
{
  return SRSRAN_SUCCESS;
}

void rrc_nr::write_dl_info(uint16_t rnti, srsran::unique_byte_buffer_t sdu) {}

/*******************************************************************************
  Interface for EUTRA RRC
*******************************************************************************/

void rrc_nr::sgnb_addition_request(uint16_t eutra_rnti, const sgnb_addition_req_params_t& params)
{
  // try to allocate new user
  sched_nr_ue_cfg_t uecfg{};
  uecfg.carriers.resize(1);
  uecfg.carriers[0].active      = true;
  uecfg.carriers[0].cc          = 0;
  uecfg.ue_bearers[0].direction = mac_lc_ch_cfg_t::BOTH;
  srsran::phy_cfg_nr_default_t::reference_cfg_t ref_args{};
  ref_args.duplex = cfg.cell_list[0].duplex_mode == SRSRAN_DUPLEX_MODE_TDD
                        ? srsran::phy_cfg_nr_default_t::reference_cfg_t::R_DUPLEX_TDD_CUSTOM_6_4
                        : srsran::phy_cfg_nr_default_t::reference_cfg_t::R_DUPLEX_FDD;
  uecfg.phy_cfg     = srsran::phy_cfg_nr_default_t{ref_args};
  uecfg.phy_cfg.csi = {}; // disable CSI until RA is complete

  uint16_t nr_rnti = mac->reserve_rnti(0, uecfg);
  if (nr_rnti == SRSRAN_INVALID_RNTI) {
    logger.error("Failed to allocate RNTI at MAC");
    rrc_eutra->sgnb_addition_reject(eutra_rnti);
    return;
  }

  if (add_user(nr_rnti, uecfg, false) != SRSRAN_SUCCESS) {
    logger.error("Failed to allocate RNTI at RRC");
    rrc_eutra->sgnb_addition_reject(eutra_rnti);
    return;
  }

  // new RNTI is now registered at MAC and RRC
  auto user_it = users.find(nr_rnti);
  if (user_it == users.end()) {
    logger.warning("Unrecognised rnti: 0x%x", nr_rnti);
    return;
  }
  user_it->second->handle_sgnb_addition_request(eutra_rnti, params);
}

void rrc_nr::sgnb_reconfiguration_complete(uint16_t eutra_rnti, const asn1::dyn_octstring& reconfig_response)
{
  // user has completeted the reconfiguration and has acked on 4G side, wait until RA on NR
  logger.info("Received Reconfiguration complete for RNTI=0x%x", eutra_rnti);
}

void rrc_nr::sgnb_release_request(uint16_t nr_rnti)
{
  // remove user
  auto     it         = users.find(nr_rnti);
  uint16_t eutra_rnti = it != users.end() ? it->second->get_eutra_rnti() : SRSRAN_INVALID_RNTI;
  rem_user(nr_rnti);
  if (eutra_rnti != SRSRAN_INVALID_RNTI) {
    rrc_eutra->sgnb_release_ack(eutra_rnti);
  }
}

/*******************************************************************************
  UE class

  Every function in UE class is called from a mutex environment thus does not
  need extra protection.
*******************************************************************************/
rrc_nr::ue::ue(rrc_nr* parent_, uint16_t rnti_, const sched_nr_ue_cfg_t& uecfg_, bool start_msg3_timer) :
  parent(parent_), rnti(rnti_), uecfg(uecfg_)
{
  // Derive UE cfg from rrc_cfg_nr_t
  uecfg.phy_cfg.pdcch = parent->cfg.cell_list[0].phy_cell.pdcch;

  // Set timer for MSG3_RX_TIMEOUT or UE_INACTIVITY_TIMEOUT
  activity_timer = parent->task_sched.get_unique_timer();
  start_msg3_timer ? set_activity_timeout(MSG3_RX_TIMEOUT) : set_activity_timeout(MSG5_RX_TIMEOUT);
}

void rrc_nr::ue::set_activity_timeout(activity_timeout_type_t type)
{
  uint32_t deadline_ms = 0;

  switch (type) {
    case MSG3_RX_TIMEOUT:
      // TODO: Retrieve the parameters from somewhere(RRC?) - Currently hardcoded to 100ms
      deadline_ms = 100;
      break;
    case MSG5_RX_TIMEOUT:
      // TODO: Retrieve the parameters from somewhere(RRC?) - Currently hardcoded to 1s
      deadline_ms = 5000;
      break;
    case UE_INACTIVITY_TIMEOUT:
      // TODO: Retrieve the parameters from somewhere(RRC?) - Currently hardcoded to 5s
      deadline_ms = 10000;
      break;
    default:
      parent->logger.error("Unknown timeout type %d", type);
      return;
  }

  activity_timer.set(deadline_ms, [this, type](uint32_t tid) { activity_timer_expired(type); });
  parent->logger.debug("Setting timer for %s for rnti=0x%x to %dms", to_string(type).c_str(), rnti, deadline_ms);

  set_activity();
}

void rrc_nr::ue::set_activity(bool enabled)
{
  if (not enabled) {
    if (activity_timer.is_running()) {
      parent->logger.debug("Inactivity timer interrupted for rnti=0x%x", rnti);
    }
    activity_timer.stop();
    return;
  }

  // re-start activity timer with current timeout value
  activity_timer.run();
  parent->logger.debug("Activity registered for rnti=0x%x (timeout_value=%dms)", rnti, activity_timer.duration());
}

void rrc_nr::ue::activity_timer_expired(const activity_timeout_type_t type)
{
  parent->logger.info("Activity timer for rnti=0x%x expired after %d ms", rnti, activity_timer.time_elapsed());

  switch (type) {
    case MSG5_RX_TIMEOUT:
    case UE_INACTIVITY_TIMEOUT:
      state = rrc_nr_state_t::RRC_INACTIVE;
      parent->rrc_eutra->sgnb_inactivity_timeout(eutra_rnti);
      break;
    case MSG3_RX_TIMEOUT: {
      // MSG3 timeout, no need to notify NGAP or LTE stack. Just remove UE
      state                = rrc_nr_state_t::RRC_IDLE;
      uint32_t rnti_to_rem = rnti;
      parent->task_sched.defer_task([this, rnti_to_rem]() { parent->rem_user(rnti_to_rem); });
      break;
    }
    default:
      // Unhandled activity timeout, just remove UE and log an error
      parent->rem_user(rnti);
      parent->logger.error(
          "Unhandled reason for activity timer expiration. rnti=0x%x, cause %d", rnti, static_cast<unsigned>(type));
  }
}

std::string rrc_nr::ue::to_string(const activity_timeout_type_t& type)
{
  constexpr static const char* options[] = {"Msg3 reception", "UE inactivity", "Msg5 reception"};
  return srsran::enum_to_text(options, (uint32_t)activity_timeout_type_t::nulltype, (uint32_t)type);
}

void rrc_nr::ue::send_dl_ccch(const dl_ccch_msg_s& dl_ccch_msg)
{
  // Allocate a new PDU buffer, pack the message and send to PDCP
  srsran::unique_byte_buffer_t pdu = parent->pack_into_pdu(dl_ccch_msg);
  if (pdu == nullptr) {
    parent->logger.error("Failed to send DL-CCCH");
    return;
  }
  fmt::memory_buffer fmtbuf;
  fmt::format_to(fmtbuf, "DL-CCCH.{}", dl_ccch_msg.msg.c1().type().to_string());
  log_rrc_message(srsran::nr_srb::srb0, Tx, *pdu.get(), dl_ccch_msg, srsran::to_c_str(fmtbuf));
  parent->rlc->write_sdu(rnti, srsran::srb_to_lcid(srsran::nr_srb::srb0), std::move(pdu));
}

template <class T>
srsran::unique_byte_buffer_t rrc_nr::pack_into_pdu(const T& msg)
{
  // Allocate a new PDU buffer and pack the
  srsran::unique_byte_buffer_t pdu = srsran::make_byte_buffer();
  if (pdu == nullptr) {
    logger.error("Couldn't allocate PDU in %s().", __FUNCTION__);
    return nullptr;
  }
  asn1::bit_ref bref(pdu->msg, pdu->get_tailroom());
  if (msg.pack(bref) == asn1::SRSASN_ERROR_ENCODE_FAIL) {
    logger.error("Failed to pack message. Discarding it.");
    return nullptr;
  }
  pdu->N_bytes = bref.distance_bytes();
  return pdu;
}

int rrc_nr::ue::pack_secondary_cell_group_rlc_cfg(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // RLC for DRB1 (with fixed LCID)
  cell_group_cfg_pack.rlc_bearer_to_add_mod_list_present = true;
  cell_group_cfg_pack.rlc_bearer_to_add_mod_list.resize(1);
  auto& rlc_bearer                       = cell_group_cfg_pack.rlc_bearer_to_add_mod_list[0];
  rlc_bearer.lc_ch_id                    = drb1_lcid;
  rlc_bearer.served_radio_bearer_present = true;
  rlc_bearer.served_radio_bearer.set_drb_id();
  rlc_bearer.served_radio_bearer.drb_id() = 1;
  rlc_bearer.rlc_cfg_present              = true;
  rlc_bearer.rlc_cfg.set_um_bi_dir();
  rlc_bearer.rlc_cfg.um_bi_dir().ul_um_rlc.sn_field_len_present = true;
  rlc_bearer.rlc_cfg.um_bi_dir().ul_um_rlc.sn_field_len         = sn_field_len_um_opts::size12;
  rlc_bearer.rlc_cfg.um_bi_dir().dl_um_rlc.sn_field_len_present = true;
  rlc_bearer.rlc_cfg.um_bi_dir().dl_um_rlc.sn_field_len         = sn_field_len_um_opts::size12;
  rlc_bearer.rlc_cfg.um_bi_dir().dl_um_rlc.t_reassembly         = t_reassembly_opts::ms50;

  // MAC logical channel config
  rlc_bearer.mac_lc_ch_cfg_present                    = true;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params_present = true;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.prio    = 11;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.prioritised_bit_rate =
      asn1::rrc_nr::lc_ch_cfg_s::ul_specific_params_s_::prioritised_bit_rate_opts::kbps0;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.bucket_size_dur =
      asn1::rrc_nr::lc_ch_cfg_s::ul_specific_params_s_::bucket_size_dur_opts::ms100;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.lc_ch_group_present      = true;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.lc_ch_group              = 6;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.sched_request_id_present = true;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.sched_request_id         = 0;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_secondary_cell_group_mac_cfg(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // mac-CellGroup-Config for BSR and SR
  cell_group_cfg_pack.mac_cell_group_cfg_present                         = true;
  auto& mac_cell_group                                                   = cell_group_cfg_pack.mac_cell_group_cfg;
  mac_cell_group.sched_request_cfg_present                               = true;
  mac_cell_group.sched_request_cfg.sched_request_to_add_mod_list_present = true;
  mac_cell_group.sched_request_cfg.sched_request_to_add_mod_list.resize(1);
  mac_cell_group.sched_request_cfg.sched_request_to_add_mod_list[0].sched_request_id = 0;
  mac_cell_group.sched_request_cfg.sched_request_to_add_mod_list[0].sr_trans_max =
      asn1::rrc_nr::sched_request_to_add_mod_s::sr_trans_max_opts::n64;
  mac_cell_group.bsr_cfg_present            = true;
  mac_cell_group.bsr_cfg.periodic_bsr_timer = asn1::rrc_nr::bsr_cfg_s::periodic_bsr_timer_opts::sf20;
  mac_cell_group.bsr_cfg.retx_bsr_timer     = asn1::rrc_nr::bsr_cfg_s::retx_bsr_timer_opts::sf320;

  // Skip TAG and PHR config
  mac_cell_group.tag_cfg_present                     = false;
  mac_cell_group.tag_cfg.tag_to_add_mod_list_present = true;
  mac_cell_group.tag_cfg.tag_to_add_mod_list.resize(1);
  mac_cell_group.tag_cfg.tag_to_add_mod_list[0].tag_id           = 0;
  mac_cell_group.tag_cfg.tag_to_add_mod_list[0].time_align_timer = time_align_timer_opts::infinity;

  mac_cell_group.phr_cfg_present = false;
  mac_cell_group.phr_cfg.set_setup();
  mac_cell_group.phr_cfg.setup().phr_periodic_timer       = asn1::rrc_nr::phr_cfg_s::phr_periodic_timer_opts::sf500;
  mac_cell_group.phr_cfg.setup().phr_prohibit_timer       = asn1::rrc_nr::phr_cfg_s::phr_prohibit_timer_opts::sf200;
  mac_cell_group.phr_cfg.setup().phr_tx_pwr_factor_change = asn1::rrc_nr::phr_cfg_s::phr_tx_pwr_factor_change_opts::db3;
  mac_cell_group.phr_cfg.setup().multiple_phr             = true;
  mac_cell_group.phr_cfg.setup().dummy                    = false;
  mac_cell_group.phr_cfg.setup().phr_type2_other_cell     = false;
  mac_cell_group.phr_cfg.setup().phr_mode_other_cg        = asn1::rrc_nr::phr_cfg_s::phr_mode_other_cg_opts::real;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded_init_dl_bwp(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.init_dl_bwp_present = true;

  pack_sp_cell_cfg_ded_init_dl_bwp_pdsch_cfg(cell_group_cfg_pack);
  pack_sp_cell_cfg_ded_init_dl_bwp_radio_link_monitoring(cell_group_cfg_pack);

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded_init_dl_bwp_radio_link_monitoring(
    asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.init_dl_bwp.radio_link_monitoring_cfg_present = true;
  auto& radio_link_monitoring = cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.init_dl_bwp.radio_link_monitoring_cfg;
  radio_link_monitoring.set_setup().fail_detection_res_to_add_mod_list_present = true;

  // add resource to detect RLF
  radio_link_monitoring.set_setup().fail_detection_res_to_add_mod_list.resize(1);
  auto& fail_detec_res_elem = radio_link_monitoring.set_setup().fail_detection_res_to_add_mod_list[0];
  fail_detec_res_elem.radio_link_monitoring_rs_id = 0;
  fail_detec_res_elem.purpose                     = asn1::rrc_nr::radio_link_monitoring_rs_s::purpose_opts::rlf;
  fail_detec_res_elem.detection_res.set_ssb_idx() = 0;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded_init_dl_bwp_pdsch_cfg(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.init_dl_bwp.pdsch_cfg_present = true;
  auto& pdsch_cfg_dedicated = cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.init_dl_bwp.pdsch_cfg;

  pdsch_cfg_dedicated.set_setup();
  pdsch_cfg_dedicated.setup().dmrs_dl_for_pdsch_map_type_a_present = true;
  pdsch_cfg_dedicated.setup().dmrs_dl_for_pdsch_map_type_a.set_setup();
  pdsch_cfg_dedicated.setup().dmrs_dl_for_pdsch_map_type_a.setup().dmrs_add_position_present = true;
  pdsch_cfg_dedicated.setup().dmrs_dl_for_pdsch_map_type_a.setup().dmrs_add_position =
      asn1::rrc_nr::dmrs_dl_cfg_s::dmrs_add_position_opts::pos1;
  pdsch_cfg_dedicated.setup().tci_states_to_add_mod_list_present = true;
  pdsch_cfg_dedicated.setup().tci_states_to_add_mod_list.resize(1);
  pdsch_cfg_dedicated.setup().tci_states_to_add_mod_list[0].tci_state_id = 0;
  pdsch_cfg_dedicated.setup().tci_states_to_add_mod_list[0].qcl_type1.ref_sig.set_ssb();
  pdsch_cfg_dedicated.setup().tci_states_to_add_mod_list[0].qcl_type1.ref_sig.ssb() = 0;
  pdsch_cfg_dedicated.setup().tci_states_to_add_mod_list[0].qcl_type1.qcl_type =
      asn1::rrc_nr::qcl_info_s::qcl_type_opts::type_d;
  pdsch_cfg_dedicated.setup().res_alloc = pdsch_cfg_s::res_alloc_opts::res_alloc_type1;
  pdsch_cfg_dedicated.setup().rbg_size  = asn1::rrc_nr::pdsch_cfg_s::rbg_size_opts::cfg1;
  pdsch_cfg_dedicated.setup().prb_bundling_type.set_static_bundling();
  pdsch_cfg_dedicated.setup().prb_bundling_type.static_bundling().bundle_size_present = true;
  pdsch_cfg_dedicated.setup().prb_bundling_type.static_bundling().bundle_size =
      asn1::rrc_nr::pdsch_cfg_s::prb_bundling_type_c_::static_bundling_s_::bundle_size_opts::wideband;

  // ZP-CSI
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list_present = false;
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list.resize(1);
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].zp_csi_rs_res_id = 0;
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].res_map.freq_domain_alloc.set_row4();
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].res_map.freq_domain_alloc.row4().from_number(0b100);
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].res_map.nrof_ports =
      asn1::rrc_nr::csi_rs_res_map_s::nrof_ports_opts::p4;

  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].res_map.first_ofdm_symbol_in_time_domain = 8;
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].res_map.cdm_type =
      asn1::rrc_nr::csi_rs_res_map_s::cdm_type_opts::fd_cdm2;
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].res_map.density.set_one();

  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].res_map.freq_band.start_rb     = 0;
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].res_map.freq_band.nrof_rbs     = 52;
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].periodicity_and_offset_present = true;
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].periodicity_and_offset.set_slots80();
  pdsch_cfg_dedicated.setup().zp_csi_rs_res_to_add_mod_list[0].periodicity_and_offset.slots80() = 1;
  pdsch_cfg_dedicated.setup().p_zp_csi_rs_res_set_present                                       = false;
  pdsch_cfg_dedicated.setup().p_zp_csi_rs_res_set.set_setup();
  pdsch_cfg_dedicated.setup().p_zp_csi_rs_res_set.setup().zp_csi_rs_res_set_id = 0;
  pdsch_cfg_dedicated.setup().p_zp_csi_rs_res_set.setup().zp_csi_rs_res_id_list.resize(1);

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded_ul_cfg_init_ul_bwp_pucch_cfg(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // PUCCH
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg.init_ul_bwp.pucch_cfg_present = true;
  auto& pucch_cfg = cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg.init_ul_bwp.pucch_cfg;

  pucch_cfg.set_setup();
  pucch_cfg.setup().format2_present = true;
  pucch_cfg.setup().format2.set_setup();
  pucch_cfg.setup().format2.setup().max_code_rate_present = true;
  pucch_cfg.setup().format2.setup().max_code_rate         = pucch_max_code_rate_opts::zero_dot25;

  // SR resources
  pucch_cfg.setup().sched_request_res_to_add_mod_list_present = true;
  pucch_cfg.setup().sched_request_res_to_add_mod_list.resize(1);
  auto& sr_res1                             = pucch_cfg.setup().sched_request_res_to_add_mod_list[0];
  sr_res1.sched_request_res_id              = 1;
  sr_res1.sched_request_id                  = 0;
  sr_res1.periodicity_and_offset_present    = true;
  sr_res1.periodicity_and_offset.set_sl40() = 8;
  sr_res1.res_present                       = true;
  sr_res1.res                               = 2; // PUCCH resource for SR

  // DL data
  pucch_cfg.setup().dl_data_to_ul_ack_present = true;

  if (parent->cfg.cell_list[0].duplex_mode == SRSRAN_DUPLEX_MODE_FDD) {
    pucch_cfg.setup().dl_data_to_ul_ack.resize(1);
    pucch_cfg.setup().dl_data_to_ul_ack[0] = 4;
  } else {
    pucch_cfg.setup().dl_data_to_ul_ack.resize(6);
    pucch_cfg.setup().dl_data_to_ul_ack[0] = 6;
    pucch_cfg.setup().dl_data_to_ul_ack[1] = 5;
    pucch_cfg.setup().dl_data_to_ul_ack[2] = 4;
    pucch_cfg.setup().dl_data_to_ul_ack[3] = 4;
    pucch_cfg.setup().dl_data_to_ul_ack[4] = 4;
    pucch_cfg.setup().dl_data_to_ul_ack[5] = 4;
  }

  // PUCCH Resource for format 1
  srsran_pucch_nr_resource_t resource_small = {};
  resource_small.starting_prb               = 0;
  resource_small.format                     = SRSRAN_PUCCH_NR_FORMAT_1;
  resource_small.initial_cyclic_shift       = 0;
  resource_small.nof_symbols                = 14;
  resource_small.start_symbol_idx           = 0;
  resource_small.time_domain_occ            = 0;

  // PUCCH Resource for format 2
  srsran_pucch_nr_resource_t resource_big = {};
  resource_big.starting_prb               = 51;
  resource_big.format                     = SRSRAN_PUCCH_NR_FORMAT_2;
  resource_big.nof_prb                    = 1;
  resource_big.nof_symbols                = 2;
  resource_big.start_symbol_idx           = 12;

  // Resource for SR
  srsran_pucch_nr_resource_t resource_sr = {};
  resource_sr.starting_prb               = 51;
  resource_sr.format                     = SRSRAN_PUCCH_NR_FORMAT_1;
  resource_sr.initial_cyclic_shift       = 0;
  resource_sr.nof_symbols                = 14;
  resource_sr.start_symbol_idx           = 0;
  resource_sr.time_domain_occ            = 0;

  // Make 3 possible resources
  pucch_cfg.setup().res_to_add_mod_list_present = true;
  pucch_cfg.setup().res_to_add_mod_list.resize(3);
  if (not srsran::make_phy_res_config(resource_small, pucch_cfg.setup().res_to_add_mod_list[0], 0)) {
    parent->logger.warning("Failed to create 1-2 bit NR PUCCH resource");
  }
  if (not srsran::make_phy_res_config(resource_big, pucch_cfg.setup().res_to_add_mod_list[1], 1)) {
    parent->logger.warning("Failed to create >2 bit NR PUCCH resource");
  }
  if (not srsran::make_phy_res_config(resource_sr, pucch_cfg.setup().res_to_add_mod_list[2], 2)) {
    parent->logger.warning("Failed to create SR NR PUCCH resource");
  }

  // Make 2 PUCCH resource sets
  pucch_cfg.setup().res_set_to_add_mod_list_present = true;
  pucch_cfg.setup().res_set_to_add_mod_list.resize(2);

  // Make PUCCH resource set for 1-2 bit
  pucch_cfg.setup().res_set_to_add_mod_list[0].pucch_res_set_id = 0;
  pucch_cfg.setup().res_set_to_add_mod_list[0].res_list.resize(8);
  for (auto& e : pucch_cfg.setup().res_set_to_add_mod_list[0].res_list) {
    e = 0;
  }

  // Make PUCCH resource set for >2 bit
  pucch_cfg.setup().res_set_to_add_mod_list[1].pucch_res_set_id = 1;
  pucch_cfg.setup().res_set_to_add_mod_list[1].res_list.resize(8);
  for (auto& e : pucch_cfg.setup().res_set_to_add_mod_list[1].res_list) {
    e = 1;
  }

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded_ul_cfg_init_ul_bwp_pusch_cfg(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // PUSCH config
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg.init_ul_bwp.pusch_cfg_present = true;
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg.init_ul_bwp.pusch_cfg.set_setup();
  auto& pusch_cfg_ded = cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg.init_ul_bwp.pusch_cfg.setup();

  pusch_cfg_ded.dmrs_ul_for_pusch_map_type_a_present = true;
  pusch_cfg_ded.dmrs_ul_for_pusch_map_type_a.set_setup();
  pusch_cfg_ded.dmrs_ul_for_pusch_map_type_a.setup().dmrs_add_position_present = true;
  pusch_cfg_ded.dmrs_ul_for_pusch_map_type_a.setup().dmrs_add_position = dmrs_ul_cfg_s::dmrs_add_position_opts::pos1;
  // PUSH power control skipped
  pusch_cfg_ded.res_alloc = pusch_cfg_s::res_alloc_opts::res_alloc_type1;

  // UCI
  pusch_cfg_ded.uci_on_pusch_present = true;
  pusch_cfg_ded.uci_on_pusch.set_setup();
  pusch_cfg_ded.uci_on_pusch.setup().beta_offsets_present = true;
  pusch_cfg_ded.uci_on_pusch.setup().beta_offsets.set_semi_static();
  auto& beta_offset_semi_static                        = pusch_cfg_ded.uci_on_pusch.setup().beta_offsets.semi_static();
  beta_offset_semi_static.beta_offset_ack_idx1_present = true;
  beta_offset_semi_static.beta_offset_ack_idx1         = 9;
  beta_offset_semi_static.beta_offset_ack_idx2_present = true;
  beta_offset_semi_static.beta_offset_ack_idx2         = 9;
  beta_offset_semi_static.beta_offset_ack_idx3_present = true;
  beta_offset_semi_static.beta_offset_ack_idx3         = 9;
  beta_offset_semi_static.beta_offset_csi_part1_idx1_present = true;
  beta_offset_semi_static.beta_offset_csi_part1_idx1         = 6;
  beta_offset_semi_static.beta_offset_csi_part1_idx2_present = true;
  beta_offset_semi_static.beta_offset_csi_part1_idx2         = 6;
  beta_offset_semi_static.beta_offset_csi_part2_idx1_present = true;
  beta_offset_semi_static.beta_offset_csi_part2_idx1         = 6;
  beta_offset_semi_static.beta_offset_csi_part2_idx2_present = true;
  beta_offset_semi_static.beta_offset_csi_part2_idx2         = 6;
  pusch_cfg_ded.uci_on_pusch.setup().scaling                 = uci_on_pusch_s::scaling_opts::f1;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded_ul_cfg_init_ul_bwp(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg.init_ul_bwp_present = true;

  pack_sp_cell_cfg_ded_ul_cfg_init_ul_bwp_pucch_cfg(cell_group_cfg_pack);
  pack_sp_cell_cfg_ded_ul_cfg_init_ul_bwp_pusch_cfg(cell_group_cfg_pack);

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded_ul_cfg(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // UL config dedicated
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg_present = true;

  pack_sp_cell_cfg_ded_ul_cfg_init_ul_bwp(cell_group_cfg_pack);

  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg.first_active_ul_bwp_id_present = true;
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.ul_cfg.first_active_ul_bwp_id         = 0;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded_pdcch_serving_cell_cfg(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.pdcch_serving_cell_cfg_present = true;
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.pdcch_serving_cell_cfg.set_setup();

  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.pdsch_serving_cell_cfg_present = true;
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.pdsch_serving_cell_cfg.set_setup();
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.pdsch_serving_cell_cfg.setup().nrof_harq_processes_for_pdsch_present =
      true;
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.pdsch_serving_cell_cfg.setup().nrof_harq_processes_for_pdsch =
      pdsch_serving_cell_cfg_s::nrof_harq_processes_for_pdsch_opts::n16;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_sp_cell_cfg_ded(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // SP Cell Dedicated config
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded_present                        = true;
  cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.first_active_dl_bwp_id_present = true;

  if (parent->cfg.cell_list[0].duplex_mode == SRSRAN_DUPLEX_MODE_FDD) {
    cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.first_active_dl_bwp_id = 0;
  } else {
    cell_group_cfg_pack.sp_cell_cfg.sp_cell_cfg_ded.first_active_dl_bwp_id = 1;
  }

  pack_sp_cell_cfg_ded_ul_cfg(cell_group_cfg_pack);
  pack_sp_cell_cfg_ded_init_dl_bwp(cell_group_cfg_pack);

  // Serving cell config (only to setup)
  pack_sp_cell_cfg_ded_pdcch_serving_cell_cfg(cell_group_cfg_pack);

  // spCellConfig
  if (fill_sp_cell_cfg_from_enb_cfg(parent->cfg, UE_PSCELL_CC_IDX, cell_group_cfg_pack.sp_cell_cfg) != SRSRAN_SUCCESS) {
    parent->logger.error("Failed to pack spCellConfig for rnti=0x%x", rnti);
  }

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_recfg_with_sync_sp_cell_cfg_common_dl_cfg_common_phy_cell_group_cfg(
    asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.phys_cell_group_cfg_present = true;
  cell_group_cfg_pack.phys_cell_group_cfg.pdsch_harq_ack_codebook =
      phys_cell_group_cfg_s::pdsch_harq_ack_codebook_opts::dynamic_value;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_recfg_with_sync_sp_cell_cfg_common_dl_cfg_init_dl_bwp_pdsch_cfg_common(
    asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // PDSCH config common
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.dl_cfg_common.init_dl_bwp
      .pdsch_cfg_common_present = true;
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.dl_cfg_common.init_dl_bwp.pdsch_cfg_common
      .set_setup();

  auto& pdsch_cfg_common = cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.dl_cfg_common.init_dl_bwp
                               .pdsch_cfg_common.setup();
  pdsch_cfg_common.pdsch_time_domain_alloc_list_present = true;
  pdsch_cfg_common.pdsch_time_domain_alloc_list.resize(1);
  pdsch_cfg_common.pdsch_time_domain_alloc_list[0].map_type = pdsch_time_domain_res_alloc_s::map_type_opts::type_a;
  pdsch_cfg_common.pdsch_time_domain_alloc_list[0].start_symbol_and_len = 40;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_recfg_with_sync_sp_cell_cfg_common_dl_cfg_init_dl_bwp(
    asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.dl_cfg_common.init_dl_bwp_present = true;
  auto& init_dl_bwp = cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.dl_cfg_common.init_dl_bwp;

  init_dl_bwp.generic_params.location_and_bw    = 14025;
  init_dl_bwp.generic_params.subcarrier_spacing = subcarrier_spacing_opts::khz15;

  pack_recfg_with_sync_sp_cell_cfg_common_dl_cfg_init_dl_bwp_pdsch_cfg_common(cell_group_cfg_pack);

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_recfg_with_sync_sp_cell_cfg_common_dl_cfg_common(
    asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // DL config
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.dl_cfg_common_present = true;

  pack_recfg_with_sync_sp_cell_cfg_common_dl_cfg_common_phy_cell_group_cfg(cell_group_cfg_pack);
  pack_recfg_with_sync_sp_cell_cfg_common_dl_cfg_init_dl_bwp(cell_group_cfg_pack);

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_recfg_with_sync_sp_cell_cfg_common_ul_cfg_common_init_ul_bwp_pusch_cfg_common(
    asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // PUSCH config common
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common.init_ul_bwp
      .pusch_cfg_common_present = true;
  auto& pusch_cfg_common_pack =
      cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common.init_ul_bwp.pusch_cfg_common;
  pusch_cfg_common_pack.set_setup();
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list_present = true;
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list.resize(2);
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list[0].k2_present = true;
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list[0].k2         = 4;
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list[0].map_type =
      asn1::rrc_nr::pusch_time_domain_res_alloc_s::map_type_opts::type_a;
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list[0].start_symbol_and_len = 27;
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list[1].k2_present           = true;
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list[1].k2                   = 3;
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list[1].map_type =
      asn1::rrc_nr::pusch_time_domain_res_alloc_s::map_type_opts::type_a;
  pusch_cfg_common_pack.setup().pusch_time_domain_alloc_list[1].start_symbol_and_len = 27;
  pusch_cfg_common_pack.setup().p0_nominal_with_grant_present                        = true;
  pusch_cfg_common_pack.setup().p0_nominal_with_grant                                = -60;

  // PUCCH config common
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common.init_ul_bwp
      .pucch_cfg_common_present = true;
  auto& pucch_cfg_common_pack =
      cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common.init_ul_bwp.pucch_cfg_common;
  pucch_cfg_common_pack.set_setup();
  pucch_cfg_common_pack.setup().pucch_group_hop    = asn1::rrc_nr::pucch_cfg_common_s::pucch_group_hop_opts::neither;
  pucch_cfg_common_pack.setup().p0_nominal_present = true;
  pucch_cfg_common_pack.setup().p0_nominal         = -60;

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_recfg_with_sync_sp_cell_cfg_common_ul_cfg_common_init_ul_bwp(
    asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common.init_ul_bwp_present = true;
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common.init_ul_bwp.generic_params
      .location_and_bw = 14025;
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common.init_ul_bwp.generic_params
      .subcarrier_spacing = subcarrier_spacing_opts::khz15;

  pack_recfg_with_sync_sp_cell_cfg_common_ul_cfg_common_init_ul_bwp_pusch_cfg_common(cell_group_cfg_pack);

  return SRSRAN_ERROR;
}

int rrc_nr::ue::pack_recfg_with_sync_sp_cell_cfg_common_ul_cfg_common(
    asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // UL config
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common_present = true;
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common.ul_cfg_common.dummy = time_align_timer_opts::ms500;

  pack_recfg_with_sync_sp_cell_cfg_common_ul_cfg_common_init_ul_bwp(cell_group_cfg_pack);

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_recfg_with_sync_sp_cell_cfg_common(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  auto& pscell_cfg = parent->cfg.cell_list.at(UE_PSCELL_CC_IDX);

  if (pscell_cfg.duplex_mode == SRSRAN_DUPLEX_MODE_TDD) {
    cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.smtc.release();
  }

  // DL config
  pack_recfg_with_sync_sp_cell_cfg_common_dl_cfg_common(cell_group_cfg_pack);

  // UL config
  pack_recfg_with_sync_sp_cell_cfg_common_ul_cfg_common(cell_group_cfg_pack);

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_recfg_with_sync(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  // Reconfig with Sync
  cell_group_cfg_pack.cell_group_id = 1; // 0 identifies the MCG. Other values identify SCGs.

  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync_present   = true;
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.new_ue_id = rnti;
  cell_group_cfg_pack.sp_cell_cfg.recfg_with_sync.t304      = recfg_with_sync_s::t304_opts::ms1000;

  pack_recfg_with_sync_sp_cell_cfg_common(cell_group_cfg_pack);

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::pack_secondary_cell_group_sp_cell_cfg(asn1::rrc_nr::cell_group_cfg_s& cell_group_cfg_pack)
{
  cell_group_cfg_pack.sp_cell_cfg_present               = true;
  cell_group_cfg_pack.sp_cell_cfg.serv_cell_idx_present = true;
  cell_group_cfg_pack.sp_cell_cfg.serv_cell_idx = 1; // Serving cell ID of a PSCell. The PCell of the MCG uses ID 0.

  pack_sp_cell_cfg_ded(cell_group_cfg_pack);
  pack_recfg_with_sync(cell_group_cfg_pack);

  return SRSRAN_SUCCESS;
}

// Helper for the RRC Reconfiguration sender to pack hard-coded config
int rrc_nr::ue::pack_secondary_cell_group_cfg(asn1::dyn_octstring& packed_secondary_cell_config)
{
  auto& cell_group_cfg_pack = cell_group_cfg;

  pack_secondary_cell_group_rlc_cfg(cell_group_cfg_pack);
  pack_secondary_cell_group_mac_cfg(cell_group_cfg_pack);
  pack_secondary_cell_group_sp_cell_cfg(cell_group_cfg_pack);

  // make sufficiant space
  packed_secondary_cell_config.resize(256);
  asn1::bit_ref bref_pack(packed_secondary_cell_config.data(), packed_secondary_cell_config.size());
  if (cell_group_cfg_pack.pack(bref_pack) != asn1::SRSASN_SUCCESS) {
    parent->logger.error("Failed to pack NR secondary cell config");
    return SRSRAN_ERROR;
  }
  packed_secondary_cell_config.resize(bref_pack.distance_bytes());

  log_rrc_container(Tx, packed_secondary_cell_config, cell_group_cfg_pack, "nr-SecondaryCellGroupConfig-r15");

  return SRSRAN_SUCCESS;
}

// Packs a hard-coded RRC Reconfiguration with fixed params for all layers (for now)
int rrc_nr::ue::pack_rrc_reconfiguration(asn1::dyn_octstring& packed_rrc_reconfig)
{
  rrc_recfg_s reconfig;
  reconfig.rrc_transaction_id = ((transaction_id++) % 4u);
  rrc_recfg_ies_s& recfg_ies  = reconfig.crit_exts.set_rrc_recfg();

  // add secondary cell group config
  recfg_ies.secondary_cell_group_present = true;

  if (pack_secondary_cell_group_cfg(recfg_ies.secondary_cell_group) == SRSRAN_ERROR) {
    parent->logger.error("Failed to pack secondary cell group");
    return SRSRAN_ERROR;
  }

  // now pack ..
  packed_rrc_reconfig.resize(512);
  asn1::bit_ref bref_pack(packed_rrc_reconfig.data(), packed_rrc_reconfig.size());
  if (reconfig.pack(bref_pack) != asn1::SRSASN_SUCCESS) {
    parent->logger.error("Failed to pack RRC Reconfiguration");
    return SRSRAN_ERROR;
  }
  packed_rrc_reconfig.resize(bref_pack.distance_bytes());

  return SRSRAN_SUCCESS;
}

// Packs a hard-coded NR radio bearer config with fixed params for RLC/PDCP (for now)
int rrc_nr::ue::pack_nr_radio_bearer_config(asn1::dyn_octstring& packed_nr_bearer_config)
{
  // set security config
  auto& radio_bearer_cfg_pack                        = radio_bearer_cfg;
  radio_bearer_cfg_pack.security_cfg_present         = true;
  auto& sec_cfg                                      = radio_bearer_cfg_pack.security_cfg;
  sec_cfg.key_to_use_present                         = true;
  sec_cfg.key_to_use                                 = asn1::rrc_nr::security_cfg_s::key_to_use_opts::secondary;
  sec_cfg.security_algorithm_cfg_present             = true;
  sec_cfg.security_algorithm_cfg.ciphering_algorithm = ciphering_algorithm_opts::nea0;
  sec_cfg.security_algorithm_cfg.integrity_prot_algorithm_present = true;
  sec_cfg.security_algorithm_cfg.integrity_prot_algorithm         = integrity_prot_algorithm_opts::nia0;

  // pack it
  packed_nr_bearer_config.resize(128);
  asn1::bit_ref bref_pack(packed_nr_bearer_config.data(), packed_nr_bearer_config.size());
  if (radio_bearer_cfg_pack.pack(bref_pack) != asn1::SRSASN_SUCCESS) {
    parent->logger.error("Failed to pack NR radio bearer config");
    return SRSRAN_ERROR;
  }

  // resize to packed length
  packed_nr_bearer_config.resize(bref_pack.distance_bytes());

  log_rrc_container(Tx, packed_nr_bearer_config, radio_bearer_cfg_pack, "nr-RadioBearerConfig1-r15");

  return SRSRAN_SUCCESS;
}

int rrc_nr::ue::handle_sgnb_addition_request(uint16_t eutra_rnti_, const sgnb_addition_req_params_t& req_params)
{
  // Add DRB1 to RLC and PDCP
  if (add_drb() != SRSRAN_SUCCESS) {
    parent->logger.error("Failed to configure DRB");
    parent->rrc_eutra->sgnb_addition_reject(eutra_rnti_);
    return SRSRAN_ERROR;
  }

  // provide hard-coded NR configs
  rrc_eutra_interface_rrc_nr::sgnb_addition_ack_params_t ack_params = {};
  if (pack_rrc_reconfiguration(ack_params.nr_secondary_cell_group_cfg_r15) == SRSRAN_ERROR) {
    parent->logger.error("Failed to pack RRC Reconfiguration. Sending SgNB addition reject.");
    parent->rrc_eutra->sgnb_addition_reject(eutra_rnti_);
    return SRSRAN_ERROR;
  }

  if (pack_nr_radio_bearer_config(ack_params.nr_radio_bearer_cfg1_r15) == SRSRAN_ERROR) {
    parent->logger.error("Failed to pack NR radio bearer config. Sending SgNB addition reject.");
    parent->rrc_eutra->sgnb_addition_reject(eutra_rnti_);
    return SRSRAN_ERROR;
  }

  // send response to EUTRA
  ack_params.nr_rnti       = rnti;
  ack_params.eps_bearer_id = req_params.eps_bearer_id;
  parent->rrc_eutra->sgnb_addition_ack(eutra_rnti_, ack_params);

  // recognize RNTI as ENDC user
  endc       = true;
  eutra_rnti = eutra_rnti_;

  return SRSRAN_SUCCESS;
}

void rrc_nr::ue::crnti_ce_received()
{
  // Assume NSA mode active
  if (endc) {
    // send SgNB addition complete for ENDC users
    parent->rrc_eutra->sgnb_addition_complete(eutra_rnti, rnti);

    // stop RX MSG3/MSG5 activity timer on MAC CE RNTI reception
    set_activity_timeout(UE_INACTIVITY_TIMEOUT);
    parent->logger.debug("Received MAC CE-RNTI for 0x%x - stopping MSG3/MSG5 timer, starting inactivity timer", rnti);

    // Add DRB1 to MAC
    for (auto& drb : cell_group_cfg.rlc_bearer_to_add_mod_list) {
      uecfg.ue_bearers[drb.lc_ch_id].direction = mac_lc_ch_cfg_t::BOTH;
      uecfg.ue_bearers[drb.lc_ch_id].group     = drb.mac_lc_ch_cfg.ul_specific_params.lc_ch_group;
    }

    // Update UE phy params
    srsran::make_pdsch_cfg_from_serv_cell(cell_group_cfg.sp_cell_cfg.sp_cell_cfg_ded, &uecfg.phy_cfg.pdsch);
    srsran::make_csi_cfg_from_serv_cell(cell_group_cfg.sp_cell_cfg.sp_cell_cfg_ded, &uecfg.phy_cfg.csi);
    srsran::make_phy_ssb_cfg(parent->cfg.cell_list[0].phy_cell.carrier,
                             cell_group_cfg.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common,
                             &uecfg.phy_cfg.ssb);
    srsran::make_duplex_cfg_from_serv_cell(cell_group_cfg.sp_cell_cfg.recfg_with_sync.sp_cell_cfg_common,
                                           &uecfg.phy_cfg.duplex);

    parent->mac->ue_cfg(rnti, uecfg);
  }
}

/**
 * @brief Set DRB configuration
 *
 * The function sets and configures all relavant fields for the DRB configuration (MAC, RLC, PDCP) in the
 * cellGroupConfig and also adds the bearer to the local RLC and PDCP entities.
 *
 * @return int SRSRAN_SUCCESS on success
 */
int rrc_nr::ue::add_drb()
{
  // RLC for DRB1 (with fixed LCID) inside cell_group_cfg
  auto& cell_group_cfg_pack = cell_group_cfg;

  cell_group_cfg_pack.rlc_bearer_to_add_mod_list_present = true;
  cell_group_cfg_pack.rlc_bearer_to_add_mod_list.resize(1);
  auto& rlc_bearer                       = cell_group_cfg_pack.rlc_bearer_to_add_mod_list[0];
  rlc_bearer.lc_ch_id                    = drb1_lcid;
  rlc_bearer.served_radio_bearer_present = true;
  rlc_bearer.served_radio_bearer.set_drb_id();
  rlc_bearer.served_radio_bearer.drb_id() = 1;
  rlc_bearer.rlc_cfg_present              = true;
  rlc_bearer.rlc_cfg.set_um_bi_dir();
  rlc_bearer.rlc_cfg.um_bi_dir().ul_um_rlc.sn_field_len_present = true;
  rlc_bearer.rlc_cfg.um_bi_dir().ul_um_rlc.sn_field_len         = sn_field_len_um_opts::size12;
  rlc_bearer.rlc_cfg.um_bi_dir().dl_um_rlc.sn_field_len_present = true;
  rlc_bearer.rlc_cfg.um_bi_dir().dl_um_rlc.sn_field_len         = sn_field_len_um_opts::size12;
  rlc_bearer.rlc_cfg.um_bi_dir().dl_um_rlc.t_reassembly         = t_reassembly_opts::ms50;

  // add RLC bearer
  srsran::rlc_config_t rlc_cfg;
  /// NOTE, we need to pass the radio-bearer to the rlc_config
  if (srsran::make_rlc_config_t(cell_group_cfg.rlc_bearer_to_add_mod_list[0].rlc_cfg,
                                rlc_bearer.served_radio_bearer.drb_id(),
                                &rlc_cfg) != SRSRAN_SUCCESS) {
    parent->logger.error("Failed to build RLC config");
    return SRSRAN_ERROR;
  }
  parent->rlc->add_bearer(rnti, drb1_lcid, rlc_cfg);

  // MAC logical channel config
  rlc_bearer.mac_lc_ch_cfg_present                    = true;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params_present = true;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.prio    = 11;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.prioritised_bit_rate =
      asn1::rrc_nr::lc_ch_cfg_s::ul_specific_params_s_::prioritised_bit_rate_opts::kbps0;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.bucket_size_dur =
      asn1::rrc_nr::lc_ch_cfg_s::ul_specific_params_s_::bucket_size_dur_opts::ms100;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.lc_ch_group_present      = true;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.lc_ch_group              = 3;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.sched_request_id_present = true;
  rlc_bearer.mac_lc_ch_cfg.ul_specific_params.sched_request_id         = 0;
  // TODO: add LC config to MAC

  // PDCP config goes into radio_bearer_cfg
  auto& radio_bearer_cfg_pack                       = radio_bearer_cfg;
  radio_bearer_cfg_pack.drb_to_add_mod_list_present = true;
  radio_bearer_cfg_pack.drb_to_add_mod_list.resize(1);

  // configure fixed DRB1
  auto& drb_item                                = radio_bearer_cfg_pack.drb_to_add_mod_list[0];
  drb_item.drb_id                               = 1;
  drb_item.cn_assoc_present                     = true;
  drb_item.cn_assoc.set_eps_bearer_id()         = 5;
  drb_item.pdcp_cfg_present                     = true;
  drb_item.pdcp_cfg.ciphering_disabled_present  = true;
  drb_item.pdcp_cfg.drb_present                 = true;
  drb_item.pdcp_cfg.drb.pdcp_sn_size_dl_present = true;
  drb_item.pdcp_cfg.drb.pdcp_sn_size_dl         = asn1::rrc_nr::pdcp_cfg_s::drb_s_::pdcp_sn_size_dl_opts::len18bits;
  drb_item.pdcp_cfg.drb.pdcp_sn_size_ul_present = true;
  drb_item.pdcp_cfg.drb.pdcp_sn_size_ul         = asn1::rrc_nr::pdcp_cfg_s::drb_s_::pdcp_sn_size_ul_opts::len18bits;
  drb_item.pdcp_cfg.drb.discard_timer_present   = true;
  drb_item.pdcp_cfg.drb.discard_timer           = asn1::rrc_nr::pdcp_cfg_s::drb_s_::discard_timer_opts::ms100;
  drb_item.pdcp_cfg.drb.hdr_compress.set_not_used();
  drb_item.pdcp_cfg.t_reordering_present = true;
  drb_item.pdcp_cfg.t_reordering         = asn1::rrc_nr::pdcp_cfg_s::t_reordering_opts::ms0;

  // Add DRB1 to PDCP
  srsran::pdcp_config_t pdcp_cnfg = srsran::make_drb_pdcp_config_t(drb_item.drb_id, false, drb_item.pdcp_cfg);
  parent->pdcp->add_bearer(rnti, rlc_bearer.lc_ch_id, pdcp_cnfg);

  // Note: DRB1 is only activated in the MAC when the C-RNTI CE is received

  return SRSRAN_SUCCESS;
}

void rrc_nr::ue::handle_rrc_setup_request(const asn1::rrc_nr::rrc_setup_request_s& msg)
{
  if (not parent->ngap->is_amf_connected()) {
    parent->logger.error("MME isn't connected. Sending Connection Reject");
    const uint8_t max_wait_time_secs = 16;
    send_rrc_reject(max_wait_time_secs); // See TS 38.331, RejectWaitTime
    return;
  }

  // TODO: Allocate PUCCH resources and reject if not available

  switch (msg.rrc_setup_request.ue_id.type().value) {
    case asn1::rrc_nr::init_ue_id_c::types_opts::ng_minus5_g_s_tmsi_part1:
      // TODO: communicate with NGAP
      break;
    case asn1::rrc_nr::init_ue_id_c::types_opts::random_value:
      // TODO: communicate with NGAP
      break;
    default:
      parent->logger.error("Unsupported RRCSetupRequest");
  }

  send_rrc_setup();
  set_activity_timeout(UE_INACTIVITY_TIMEOUT);
}

/// TS 38.331, RRCReject message
void rrc_nr::ue::send_rrc_reject(uint8_t reject_wait_time_secs)
{
  dl_ccch_msg_s     msg;
  rrc_reject_ies_s& reject = msg.msg.set_c1().set_rrc_reject().crit_exts.set_rrc_reject();
  if (reject_wait_time_secs > 0) {
    reject.wait_time_present = true;
    reject.wait_time         = reject_wait_time_secs;
  }
  send_dl_ccch(msg);
}

/// TS 38.331, RRCSetup
void rrc_nr::ue::send_rrc_setup()
{
  dl_ccch_msg_s msg;
  rrc_setup_s&  setup        = msg.msg.set_c1().set_rrc_setup();
  setup.rrc_transaction_id   = (uint8_t)((transaction_id++) % 4);
  rrc_setup_ies_s& setup_ies = setup.crit_exts.set_rrc_setup();

  // Fill RRC Setup
  // Note: See 5.3.5.6.3 - SRB addition/modification
  setup_ies.radio_bearer_cfg.srb_to_add_mod_list_present = true;
  setup_ies.radio_bearer_cfg.srb_to_add_mod_list.resize(1);
  srb_to_add_mod_s& srb1 = setup_ies.radio_bearer_cfg.srb_to_add_mod_list[0];
  srb1.srb_id            = 1;

  send_dl_ccch(msg);
}

/// TS 38.331, RRCSetupComplete
void rrc_nr::ue::handle_rrc_setup_complete(const asn1::rrc_nr::rrc_setup_complete_s& msg)
{
  // TODO: handle RRCSetupComplete
}

/**
 * @brief Deactivate all Bearers (MAC logical channel) for this specific RNTI
 *
 * The function iterates over the bearers or MAC logical channels and deactivates them by setting each one to IDLE
 */
void rrc_nr::ue::deactivate_bearers()
{
  // Iterate over the bearers (MAC LC CH) and set each of them to IDLE
  for (auto& ue_bearer : uecfg.ue_bearers) {
    ue_bearer.direction = mac_lc_ch_cfg_t::IDLE;
  }

  // No need to check the returned value, as the function ue_cfg will return SRSRAN_SUCCESS (it asserts if it fails)
  parent->mac->ue_cfg(rnti, uecfg);
}

template <class M>
void rrc_nr::ue::log_rrc_message(srsran::nr_srb          srb,
                                 const direction_t       dir,
                                 srsran::const_byte_span pdu,
                                 const M&                msg,
                                 const char*             msg_type)
{
  fmt::memory_buffer strbuf;
  fmt::format_to(strbuf, "rnti=0x{:x}, {}", rnti, srsran::get_srb_name(srb));
  parent->log_rrc_message(srsran::to_c_str(strbuf), Tx, pdu, msg, msg_type);
}

template <class M>
void rrc_nr::ue::log_rrc_container(const direction_t       dir,
                                   srsran::const_byte_span pdu,
                                   const M&                msg,
                                   const char*             msg_type)
{
  fmt::memory_buffer strbuf;
  fmt::format_to(strbuf, "rnti=0x{:x}, container", rnti);
  parent->log_rrc_message(srsran::to_c_str(strbuf), Tx, pdu, msg, msg_type);
}

} // namespace srsenb
