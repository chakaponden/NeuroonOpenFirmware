/**
 * @file    ic_bluetooth.c
 * @Author  Paweł Kaźmierzewski <p.kazmierzewski@inteliclinic.com>
 * @date    May, 2017
 * @brief   Brief description
 *
 * Description
 */

#include "nordic_common.h"
#include "nrf.h"
#include "app_error.h"

#include "ble.h"
#include "ble_hci.h"
#include "ble_srv_common.h"
#include "ble_advdata.h"
#include "ble_advertising.h"
#include "ble_conn_params.h"
#include "ble_db_discovery.h"

#include "softdevice_handler.h"
#include "app_timer.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "semphr.h"


#include "fstorage.h"
#include "fds.h"

#include "peer_manager.h"
#include "ble_dis.h"
#include "ic_ble_service.h"
#include "ic_service_bas.h"
#include "ic_serial.h"

#include "ble_conn_state.h"

#include "ble_cts_c.h"
#include "ble_dfu.h"

#include "ic_easy_ltc_driver.h"
#include "ic_bluetooth.h"

#define NRF_LOG_MODULE_NAME "BLE"
#define NRF_LOG_LEVEL 5
#include "nrf_log.h"
#include "nrf_log_ctrl.h"

extern void main_on_ble_evt(ble_evt_t * p_ble_evt);

#define IS_SRVC_CHANGED_CHARACT_PRESENT 1                                           /**< Include or not the service_changed characteristic. if not enabled, the server's database cannot be changed for the lifetime of the device*/

#if (NRF_SD_BLE_API_VERSION == 3)
#define NRF_BLE_MAX_MTU_SIZE            GATT_MTU_SIZE_DEFAULT                       /**< MTU size used in the softdevice enabling and to reply to a BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST event. */
#endif

#define APP_FEATURE_NOT_SUPPORTED       BLE_GATT_STATUS_ATTERR_APP_BEGIN + 2        /**< Reply when unsupported features are requested. */

#define CENTRAL_LINK_COUNT              0                                           /**< Number of central links used by the application. When changing this number remember to adjust the RAM settings*/

#define PERIPHERAL_LINK_COUNT           1                                           /**< Number of peripheral links used by the application. When changing this number remember to adjust the RAM settings*/

#define DEVICE_NAME                     "NeuroOn"                               /**< Name of device. Will be included in the advertising data. */
#define MANUFACTURER_NAME               "Inteliclinic"                              /**< Manufacturer. Will be passed to Device Information Service. */

#define DEFAULT_DFU_VERSION             "1.0"
#define DEFAULT_HARDWARE_VERSION        "1.0e"

#define APP_ADV_INTERVAL                300                                         /**< The advertising interval (in units of 0.625 ms. This value corresponds to 187.5 ms). */
#define APP_ADV_TIMEOUT_IN_SECONDS      0                                           /**< The advertising timeout in units of seconds. */

#define MIN_CONN_INTERVAL               MSEC_TO_UNITS(8, UNIT_1_25_MS)            /**< Minimum acceptable connection interval (0.1 seconds). */
#define MAX_CONN_INTERVAL               MSEC_TO_UNITS(32, UNIT_1_25_MS)            /**< Maximum acceptable connection interval (0.2 second). */
#define SLAVE_LATENCY                   0                                           /**< Slave latency. */
#define CONN_SUP_TIMEOUT                MSEC_TO_UNITS(4000, UNIT_10_MS)             /**< Connection supervisory timeout (4 seconds). */

#define FIRST_CONN_PARAMS_UPDATE_DELAY  APP_TIMER_TICKS(5000, 31)  /**< Time from initiating event (connect or start of notification) to first time sd_ble_gap_conn_param_update is called (5 seconds). */ /*[TODO] PARAMETRYZACJA "0" !!!*/
#define NEXT_CONN_PARAMS_UPDATE_DELAY   APP_TIMER_TICKS(30000, 31) /**< Time between each call to sd_ble_gap_conn_param_update after the first call (30 seconds). */ /*[TODO] PARAMETRYZACJA "0" !!!*/
#define MAX_CONN_PARAMS_UPDATE_COUNT    3                                           /**< Number of attempts before giving up the connection parameter negotiation. */

#define SEC_PARAM_BOND                  1                                           /**< Perform bonding. */
#define SEC_PARAM_MITM                  0                                           /**< Man In The Middle protection not required. */
#define SEC_PARAM_LESC                  0                                           /**< LE Secure Connections not enabled. */
#define SEC_PARAM_KEYPRESS              0                                           /**< Keypress notifications not enabled. */
#define SEC_PARAM_IO_CAPABILITIES       BLE_GAP_IO_CAPS_NONE                        /**< No I/O capabilities. */
#define SEC_PARAM_OOB                   0                                           /**< Out Of Band data not available. */
#define SEC_PARAM_MIN_KEY_SIZE          7                                           /**< Minimum encryption key size. */
#define SEC_PARAM_MAX_KEY_SIZE          16                                          /**< Maximum encryption key size. */

static uint16_t m_conn_handle = BLE_CONN_HANDLE_INVALID;                            /**< Handle of the current connection. */
static TaskHandle_t m_ble_tast_handle = NULL;
static bool m_module_initialized = false;

static bool m_ble_power_down = false;

ALLOCK_SEMAPHORE(m_ble_event_ready);

/*static SemaphoreHandle_t m_ble_event_ready = NULL;*/

/* YOUR_JOB: Declare all services structure your application is using
   static ble_xx_service_t                     m_xxs;
   static ble_yy_service_t                     m_yys;
 */

static ble_dfu_t m_dfus;/* Structure used to identify the DFU service. */

static void ble_dfu_evt_handler(ble_dfu_t * p_dfu, ble_dfu_evt_t * p_evt){
    switch (p_evt->type){
        case BLE_DFU_EVT_INDICATION_DISABLED:
            NRF_LOG_INFO("Indication for BLE_DFU is disabled\r\n");
            break;
        case BLE_DFU_EVT_INDICATION_ENABLED:
            NRF_LOG_INFO("Indication for BLE_DFU is enabled\r\n");
            break;
        case BLE_DFU_EVT_ENTERING_BOOTLOADER:
            NRF_LOG_INFO("Device is entering bootloader mode!\r\n");
            break;
        default:
            NRF_LOG_INFO("Unknown event from ble_dfu\r\n");
            break;
    }
}
static ble_uuid_t m_adv_uuids[] = {{BLE_UUID_ICCS_CHARACTERISTIC, BLE_UUID_TYPE_VENDOR_BEGIN}};
// YOUR_JOB: Use UUIDs for service(s) used in your application.
/**< Universally unique service identifiers. */

static void advertising_start(void);

/**@brief Function for handling Peer Manager events.
 *
 * @param[in] p_evt  Peer Manager event.
 */
static void pm_evt_handler(pm_evt_t const * p_evt)
{
  switch (p_evt->evt_id)
  {
    case PM_EVT_BONDED_PEER_CONNECTED:
      {
        NRF_LOG_INFO("Connected to a previously bonded device.\r\n");
      } break;

    case PM_EVT_CONN_SEC_SUCCEEDED:
      {
        NRF_LOG_INFO("Connection secured. Role: %d. conn_handle: %d, Procedure: %d\r\n",
            ble_conn_state_role(p_evt->conn_handle),
            p_evt->conn_handle,
            p_evt->params.conn_sec_succeeded.procedure);
      } break;

    case PM_EVT_CONN_SEC_FAILED:
      {
        /* Often, when securing fails, it shouldn't be restarted, for security reasons.
         * Other times, it can be restarted directly.
         * Sometimes it can be restarted, but only after changing some Security Parameters.
         * Sometimes, it cannot be restarted until the link is disconnected and reconnected.
         * Sometimes it is impossible, to secure the link, or the peer device does not support it.
         * How to handle this error is highly application dependent. */
      } break;

    case PM_EVT_CONN_SEC_CONFIG_REQ:
      {
        // Reject pairing request from an already bonded peer.
        pm_conn_sec_config_t conn_sec_config = {.allow_repairing = false};
        pm_conn_sec_config_reply(p_evt->conn_handle, &conn_sec_config);
      } break;

    case PM_EVT_STORAGE_FULL:
      {
        // Run garbage collection on the flash.
        __auto_type err_code = fds_gc();
        if (err_code == FDS_ERR_BUSY || err_code == FDS_ERR_NO_SPACE_IN_QUEUES)
        {
          // Retry.
        }
        else
        {
          APP_ERROR_CHECK(err_code);
        }
      } break;

    case PM_EVT_PEERS_DELETE_SUCCEEDED:
      {
        advertising_start();
      } break;

    case PM_EVT_LOCAL_DB_CACHE_APPLY_FAILED:
      {
        // The local database has likely changed, send service changed indications.
        pm_local_database_has_changed();
      } break;

    case PM_EVT_PEER_DATA_UPDATE_FAILED:
      {
        // Assert.
        APP_ERROR_CHECK(p_evt->params.peer_data_update_failed.error);
      } break;

    case PM_EVT_PEER_DELETE_FAILED:
      {
        // Assert.
        APP_ERROR_CHECK(p_evt->params.peer_delete_failed.error);
      } break;

    case PM_EVT_PEERS_DELETE_FAILED:
      {
        // Assert.
        APP_ERROR_CHECK(p_evt->params.peers_delete_failed_evt.error);
      } break;

    case PM_EVT_ERROR_UNEXPECTED:
      {
        // Assert.
        APP_ERROR_CHECK(p_evt->params.error_unexpected.error);
      } break;

    case PM_EVT_CONN_SEC_START:
    case PM_EVT_PEER_DATA_UPDATE_SUCCEEDED:
    case PM_EVT_PEER_DELETE_SUCCEEDED:
    case PM_EVT_LOCAL_DB_CACHE_APPLIED:
    case PM_EVT_SERVICE_CHANGED_IND_SENT:
    case PM_EVT_SERVICE_CHANGED_IND_CONFIRMED:
    default:
      break;
  }
}

/**@brief Function for the GAP initialization.
 *
 * @details This function sets up all the necessary GAP (Generic Access Profile) parameters of the
 *          device including the device name, appearance, and the preferred connection parameters.
 */
static void gap_params_init(void)
{
    uint32_t                err_code;
    ble_gap_conn_params_t   gap_conn_params;
    ble_gap_conn_sec_mode_t sec_mode;

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);

    err_code = sd_ble_gap_device_name_set(&sec_mode,
                                          (const uint8_t *)DEVICE_NAME,
                                          strlen(DEVICE_NAME));
    APP_ERROR_CHECK(err_code);


    /* YOUR_JOB: Use an appearance value matching the application's use case.
       err_code = sd_ble_gap_appearance_set(BLE_APPEARANCE_);
       APP_ERROR_CHECK(err_code); */

    memset(&gap_conn_params, 0, sizeof(gap_conn_params));

    gap_conn_params.min_conn_interval = MIN_CONN_INTERVAL;
    gap_conn_params.max_conn_interval = MAX_CONN_INTERVAL;
    gap_conn_params.slave_latency     = SLAVE_LATENCY;
    gap_conn_params.conn_sup_timeout  = CONN_SUP_TIMEOUT;

    err_code = sd_ble_gap_ppcp_set(&gap_conn_params);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for initializing services that will be used by the application.
 */
static void services_init(void)
{
  static const char _digits[] = {'0','1','2','3','4','5','6','7','8','9'};
  ble_dis_init_t dis_init;

  static uint8_t m_serial_buf[IC_CHAR_MAX_LEN];

  unsigned int _manufacturer  = __start_serial_number[IC_SERIAL_MANUFACTURER];
  unsigned int _day           = __start_serial_number[IC_SERIAL_DAY];
  unsigned int _month         = __start_serial_number[IC_SERIAL_MONTH];
  unsigned int _sn            = __start_serial_number[IC_SERIAL_SN];
  unsigned int _year          = __start_serial_number[IC_SERIAL_YEAR];

  uint8_t serial_tab[] = {
    (_manufacturer-_manufacturer%10)/10,
    _manufacturer%10,
    (_day-_day%10)/10,
    _day%10,
    (_month-_month%10)/10,
    _month%10,
    (_sn-_sn%1000)/1000,
    (_sn%1000-_sn%100)/100,
    (_sn%100-_sn%10)/10,
    _sn%10,
    (_year-_year%10)/10,
    _year%10,
    0,0
  };

  __auto_type crc = crc6_calculate(serial_tab, sizeof(serial_tab));

  serial_tab[sizeof(serial_tab)-2] = (crc - crc%10)/10;
  serial_tab[sizeof(serial_tab)-1] = crc%10;

  for(size_t i = 0; i<sizeof(serial_tab); ++i){
    snprintf((char *)(&m_serial_buf[i]), sizeof(m_serial_buf-i), "%c", _digits[serial_tab[i]]);
  }

  NRF_LOG_INFO("%s\n", (uint32_t)m_serial_buf);

  // DIS
  memset(&dis_init, 0, sizeof(ble_dis_init_t));

  dis_init.manufact_name_str.length = strlen(MANUFACTURER_NAME);
  dis_init.manufact_name_str.p_str  = (uint8_t *)MANUFACTURER_NAME;

  dis_init.sw_rev_str.length        = strlen(NEUROON_OPEN_VERSION);
  dis_init.sw_rev_str.p_str         = (uint8_t *)NEUROON_OPEN_VERSION;

  dis_init.serial_num_str.length    = strlen((char*)m_serial_buf);
  dis_init.serial_num_str.p_str     = m_serial_buf;

  dis_init.hw_rev_str.length        = strlen(DEFAULT_HARDWARE_VERSION);
  dis_init.hw_rev_str.p_str         = (uint8_t *)DEFAULT_HARDWARE_VERSION;

  dis_init.fw_rev_str.length        = strlen(DEFAULT_DFU_VERSION);
  dis_init.fw_rev_str.p_str         = (uint8_t *)DEFAULT_DFU_VERSION;
  /*dis_init.dis_attr_md*/

  BLE_GAP_CONN_SEC_MODE_SET_OPEN(&dis_init.dis_attr_md.read_perm);
  BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&dis_init.dis_attr_md.write_perm);
  __auto_type err_code = ble_dis_init(&dis_init);
  APP_ERROR_CHECK(err_code);

  ble_icbas_init();

  ble_iccs_init_t iccs_init;
  ble_iccs_init(&iccs_init);

  ble_dfu_init_t dfus_init;

  // Initialize the Device Firmware Update Service.
  memset(&dfus_init, 0, sizeof(dfus_init));

  dfus_init.evt_handler                               = ble_dfu_evt_handler;
  dfus_init.ctrl_point_security_req_write_perm        = SEC_SIGNED;
  dfus_init.ctrl_point_security_req_cccd_write_perm   = SEC_SIGNED;

  err_code = ble_dfu_init(&m_dfus, &dfus_init);
  APP_ERROR_CHECK(err_code);
    /* YOUR_JOB: Add code to initialize the services used by the application.
       uint32_t                           err_code;
       ble_xxs_init_t                     xxs_init;
       ble_yys_init_t                     yys_init;

       // Initialize XXX Service.
       memset(&xxs_init, 0, sizeof(xxs_init));

       xxs_init.evt_handler                = NULL;
       xxs_init.is_xxx_notify_supported    = true;
       xxs_init.ble_xx_initial_value.level = 100;

       err_code = ble_bas_init(&m_xxs, &xxs_init);
       APP_ERROR_CHECK(err_code);

       // Initialize YYY Service.
       memset(&yys_init, 0, sizeof(yys_init));
       yys_init.evt_handler                  = on_yys_evt;
       yys_init.ble_yy_initial_value.counter = 0;

       err_code = ble_yy_service_init(&yys_init, &yy_init);
       APP_ERROR_CHECK(err_code);
     */
}

/**@brief Function for handling the Connection Parameters Module.
 *
 * @details This function will be called for all events in the Connection Parameters Module which
 *          are passed to the application.
 *          @note All this function does is to disconnect. This could have been done by simply
 *                setting the disconnect_on_fail config parameter, but instead we use the event
 *                handler mechanism to demonstrate its use.
 *
 * @param[in] p_evt  Event received from the Connection Parameters Module.
 */
static void on_conn_params_evt(ble_conn_params_evt_t * p_evt)
{
    uint32_t err_code;

    if (p_evt->evt_type == BLE_CONN_PARAMS_EVT_FAILED)
    {
        err_code = sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_CONN_INTERVAL_UNACCEPTABLE);
        APP_ERROR_CHECK(err_code);
    }
}

/**@brief Function for handling a Connection Parameters error.
 *
 * @param[in] nrf_error  Error code containing information about what went wrong.
 */
static void conn_params_error_handler(uint32_t nrf_error)
{
    APP_ERROR_HANDLER(nrf_error);
}

/**@brief Function for initializing the Connection Parameters module.
 */
static void conn_params_init(void)
{
    uint32_t               err_code;
    ble_conn_params_init_t cp_init;

    memset(&cp_init, 0, sizeof(cp_init));

    cp_init.p_conn_params                  = NULL;
    cp_init.first_conn_params_update_delay = FIRST_CONN_PARAMS_UPDATE_DELAY;
    cp_init.next_conn_params_update_delay  = NEXT_CONN_PARAMS_UPDATE_DELAY;
    cp_init.max_conn_params_update_count   = MAX_CONN_PARAMS_UPDATE_COUNT;
    cp_init.start_on_notify_cccd_handle    = BLE_GATT_HANDLE_INVALID;
    cp_init.disconnect_on_fail             = false;
    cp_init.evt_handler                    = on_conn_params_evt;
    cp_init.error_handler                  = conn_params_error_handler;

    err_code = ble_conn_params_init(&cp_init);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for putting the chip into sleep mode.
 *
 * @note This function will not return.
 */
static void sleep_mode_enter(void)
{
  //[TODO] delete it
/*
 *    uint32_t err_code = bsp_indication_set(BSP_INDICATE_IDLE);
 *
 *    APP_ERROR_CHECK(err_code);
 *
 *    // Prepare wakeup buttons.
 *    err_code = bsp_btn_ble_sleep_mode_prepare();
 *    APP_ERROR_CHECK(err_code);
 *
 */
    // Go to system-off mode (this function will not return; wakeup will cause a reset).
    uint32_t err_code = sd_power_system_off();
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for handling advertising events.
 *
 * @details This function will be called for advertising events which are passed to the application.
 *
 * @param[in] ble_adv_evt  Advertising event.
 */
static void on_adv_evt(ble_adv_evt_t ble_adv_evt)
{
    switch (ble_adv_evt)
    {
        case BLE_ADV_EVT_FAST:
            NRF_LOG_INFO("Fast advertising\r\n");
            break;

        case BLE_ADV_EVT_IDLE:
            NRF_LOG_INFO("Advertising Idle\r\n");
            sleep_mode_enter();
            break;

        default:
            break;
    }
}

/**@brief Function for handling the Application's BLE Stack events.
 *
 * @param[in] p_ble_evt  Bluetooth stack event.
 */
static void on_ble_evt(ble_evt_t * p_ble_evt)
{
    uint32_t err_code = NRF_SUCCESS;

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_DISCONNECTED:
            NRF_LOG_INFO("Disconnected.\r\n");
            break; // BLE_GAP_EVT_DISCONNECTED

        case BLE_GAP_EVT_CONNECTED:
            NRF_LOG_INFO("Connected.\r\n");
            m_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
            break; // BLE_GAP_EVT_CONNECTED

        case BLE_GATTC_EVT_TIMEOUT:
            // Disconnect on GATT Client timeout event.
            NRF_LOG_DEBUG("GATT Client Timeout.\r\n");
            err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gattc_evt.conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break; // BLE_GATTC_EVT_TIMEOUT

        case BLE_GATTS_EVT_TIMEOUT:
            // Disconnect on GATT Server timeout event.
            NRF_LOG_DEBUG("GATT Server Timeout.\r\n");
            err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gatts_evt.conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break; // BLE_GATTS_EVT_TIMEOUT

        case BLE_EVT_USER_MEM_REQUEST:
            err_code = sd_ble_user_mem_reply(p_ble_evt->evt.gattc_evt.conn_handle, NULL);
            APP_ERROR_CHECK(err_code);
            break; // BLE_EVT_USER_MEM_REQUEST

        case BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST:
        {
            ble_gatts_evt_rw_authorize_request_t  req;
            ble_gatts_rw_authorize_reply_params_t auth_reply;

            req = p_ble_evt->evt.gatts_evt.params.authorize_request;

            if (req.type != BLE_GATTS_AUTHORIZE_TYPE_INVALID)
            {
                if ((req.request.write.op == BLE_GATTS_OP_PREP_WRITE_REQ)     ||
                    (req.request.write.op == BLE_GATTS_OP_EXEC_WRITE_REQ_NOW) ||
                    (req.request.write.op == BLE_GATTS_OP_EXEC_WRITE_REQ_CANCEL))
                {
                    if (req.type == BLE_GATTS_AUTHORIZE_TYPE_WRITE)
                    {
                        auth_reply.type = BLE_GATTS_AUTHORIZE_TYPE_WRITE;
                    }
                    else
                    {
                        auth_reply.type = BLE_GATTS_AUTHORIZE_TYPE_READ;
                    }
                    auth_reply.params.write.gatt_status = APP_FEATURE_NOT_SUPPORTED;
                    err_code = sd_ble_gatts_rw_authorize_reply(p_ble_evt->evt.gatts_evt.conn_handle,
                                                               &auth_reply);
                    APP_ERROR_CHECK(err_code);
                }
            }
        } break; // BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST

#if (NRF_SD_BLE_API_VERSION == 3)
        case BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST:
            err_code = sd_ble_gatts_exchange_mtu_reply(p_ble_evt->evt.gatts_evt.conn_handle,
                                                       NRF_BLE_MAX_MTU_SIZE);
            APP_ERROR_CHECK(err_code);
            break; // BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST
#endif

        default:
            // No implementation needed.
            break;
    }
}
/**@brief Function for dispatching a system event to interested modules.
 *
 * @details This function is called from the System event interrupt handler after a system
 *          event has been received.
 *
 * @param[in] sys_evt  System stack event.
 */
static void sys_evt_dispatch(uint32_t sys_evt)
{
  // Dispatch the system event to the fstorage module, where it will be
  // dispatched to the Flash Data Storage (FDS) module.
  // [TODO] ogarnąć file system
  fs_sys_event_handler(sys_evt);

  // Dispatch to the Advertising module last, since it will check if there are any
  // pending flash operations in fstorage. Let fstorage process system events first,
  // so that it can report correctly to the Advertising module.
  ble_advertising_on_sys_evt(sys_evt);
}

/**@brief Function for dispatching a BLE stack event to all modules with a BLE stack event handler.
 *
 * @details This function is called from the BLE Stack event interrupt handler after a BLE stack
 *          event has been received.
 *
 * @param[in] p_ble_evt  Bluetooth stack event.
 */
static void ble_evt_dispatch(ble_evt_t * p_ble_evt)
{
  /** The Connection state module has to be fed BLE events in order to function correctly
   * Remember to call ble_conn_state_on_ble_evt before calling any ble_conns_state_* functions. */
  ble_conn_state_on_ble_evt(p_ble_evt);
  pm_on_ble_evt(p_ble_evt);
  ble_conn_params_on_ble_evt(p_ble_evt);
  on_ble_evt(p_ble_evt);
  if(!m_ble_power_down){
    ble_advertising_on_ble_evt(p_ble_evt);
  }
  ble_iccs_on_ble_evt(p_ble_evt);
  main_on_ble_evt(p_ble_evt);
  ble_dfu_on_ble_evt(&m_dfus, p_ble_evt);
  ble_icbas_on_ble_evt(p_ble_evt);
}

static uint32_t ble_new_event_handler(void)
{
    // The returned value may be safely ignored, if error is returned it only means that
    // the semaphore is already given (raised).
    GIVE_SEMAPHORE(m_ble_event_ready);
    return NRF_SUCCESS;
}

/**@brief Function for initializing the BLE stack.
 *
 * @details Initializes the SoftDevice and the BLE event interrupt.
 */
static void ble_stack_init(void)
{
    uint32_t err_code;

    nrf_clock_lf_cfg_t clock_lf_cfg = { .source = NRF_CLOCK_LF_SRC_SYNTH,
      .rc_ctiv = 0,
      .rc_temp_ctiv = 0,
      .xtal_accuracy = NRF_CLOCK_LF_XTAL_ACCURACY_20_PPM};

    // Initialize the SoftDevice handler module.
    SOFTDEVICE_HANDLER_INIT(&clock_lf_cfg, ble_new_event_handler);

    ble_enable_params_t ble_enable_params;
    err_code = softdevice_enable_get_default_config(CENTRAL_LINK_COUNT,
                                                    PERIPHERAL_LINK_COUNT,
                                                    &ble_enable_params);
    APP_ERROR_CHECK(err_code);
    ble_enable_params.common_enable_params.vs_uuid_count   = 2;
    /*ble_enable_params.gatts_enable_params.attr_tab_size    = 0x580;*/

    // Check the ram settings against the used number of links
    CHECK_RAM_START_ADDR(CENTRAL_LINK_COUNT, PERIPHERAL_LINK_COUNT);

    // Enable BLE stack.
#if (NRF_SD_BLE_API_VERSION == 3)
    ble_enable_params.gatt_enable_params.att_mtu = NRF_BLE_MAX_MTU_SIZE;
#endif
    err_code = softdevice_enable(&ble_enable_params);
    APP_ERROR_CHECK(err_code);

    // Register with the SoftDevice handler module for BLE events.
    err_code = softdevice_ble_evt_handler_set(ble_evt_dispatch);
    APP_ERROR_CHECK(err_code);

    // Register with the SoftDevice handler module for BLE events.
    err_code = softdevice_sys_evt_handler_set(sys_evt_dispatch);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for the Peer Manager initialization.
 *
 * @param[in] erase_bonds  Indicates whether bonding information should be cleared from
 *                         persistent storage during initialization of the Peer Manager.
 */
static void peer_manager_init(bool erase_bonds)
{
    ble_gap_sec_params_t sec_param;

    __auto_type err_code = pm_init();
    APP_ERROR_CHECK(err_code);

    if (erase_bonds)
    {
        err_code = pm_peers_delete();
        APP_ERROR_CHECK(err_code);
    }

    memset(&sec_param, 0, sizeof(ble_gap_sec_params_t));

    // Security parameters to be used for all security procedures.
    sec_param.bond           = SEC_PARAM_BOND;
    sec_param.mitm           = SEC_PARAM_MITM;
    sec_param.lesc           = SEC_PARAM_LESC;
    sec_param.keypress       = SEC_PARAM_KEYPRESS;
    sec_param.io_caps        = SEC_PARAM_IO_CAPABILITIES;
    sec_param.oob            = SEC_PARAM_OOB;
    sec_param.min_key_size   = SEC_PARAM_MIN_KEY_SIZE;
    sec_param.max_key_size   = SEC_PARAM_MAX_KEY_SIZE;
    sec_param.kdist_own.enc  = 1;
    sec_param.kdist_own.id   = 1;
    sec_param.kdist_peer.enc = 1;
    sec_param.kdist_peer.id  = 1;

    err_code = pm_sec_params_set(&sec_param);
    APP_ERROR_CHECK(err_code);

    err_code = pm_register(pm_evt_handler);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for initializing the Advertising functionality.
 */
static void advertising_init(void)
{
    ble_advdata_t          advdata;
    ble_adv_modes_config_t options;

    // Build advertising data struct to pass into @ref ble_advertising_init.
    memset(&advdata, 0, sizeof(advdata));
    advdata.name_type               = BLE_ADVDATA_FULL_NAME;

    advdata.include_appearance      = false;
    advdata.flags                   = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;
    advdata.uuids_complete.uuid_cnt = sizeof(m_adv_uuids) / sizeof(m_adv_uuids[0]);
    advdata.uuids_complete.p_uuids  = m_adv_uuids;

    memset(&options, 0, sizeof(options));
    options.ble_adv_fast_enabled  = true;
    options.ble_adv_fast_interval = APP_ADV_INTERVAL;
    options.ble_adv_fast_timeout  = APP_ADV_TIMEOUT_IN_SECONDS;

    __auto_type err_code = ble_advertising_init(&advdata, NULL, &options, on_adv_evt, NULL);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for starting advertising.
 */
static void advertising_start(void)
{
    __auto_type err_code = ble_advertising_start(BLE_ADV_MODE_FAST);

    APP_ERROR_CHECK(err_code);
}

static void init_assets(void){
  // Initialize.
  ble_stack_init();
  peer_manager_init(true);
  gap_params_init();
  services_init();
  conn_params_init();
  advertising_init();

  __auto_type err_code = ble_advertising_start(BLE_ADV_MODE_FAST);
  APP_ERROR_CHECK(err_code);
}

static void ble_stack_thread(void * arg)
{
  UNUSED_PARAMETER(arg);

  // Initialize.
/*
 *  ble_stack_init();
 *  peer_manager_init(true);
 *  gap_params_init();
 *  advertising_init();
 *  services_init();
 *  conn_params_init();
 *
 *  __auto_type err_code = ble_advertising_start(BLE_ADV_MODE_FAST);
 *  APP_ERROR_CHECK(err_code);
 */


  while (1)
  {
    /* Wait for event from SoftDevice */
    int dummy;
    TAKE_SEMAPHORE(m_ble_event_ready, portMAX_DELAY, dummy);
    UNUSED_PARAMETER(dummy);

    // This function gets events from the SoftDevice and processes them by calling the function
    // registered by softdevice_ble_evt_handler_set during stack initialization.
    // In this code ble_evt_dispatch would be called for every event found.
    intern_softdevice_events_execute();
  }
}

ic_return_val_e ic_ble_module_init(void){
  if(m_module_initialized) return IC_SUCCESS;

  INIT_SEMAPHORE_BINARY(m_ble_event_ready);

  init_assets();
  if (m_ble_tast_handle == NULL){

    // Start execution.
    if (pdPASS != xTaskCreate(ble_stack_thread, "BLE", 384, NULL, 3, &m_ble_tast_handle))
      APP_ERROR_HANDLER(NRF_ERROR_NO_MEM);
  }
  else{
    RESUME_TASK(m_ble_tast_handle);
  }

  m_module_initialized = true;
  return IC_SUCCESS;
}

ic_return_val_e ic_bluetooth_disable(void){
  if(m_module_initialized == false) return IC_NOT_INIALIZED;

  __auto_type _err_code = IC_SUCCESS;

  m_ble_power_down = true;
  if(m_conn_handle != BLE_CONN_HANDLE_INVALID){
    sd_ble_gap_disconnect(m_conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
    m_conn_handle = BLE_CONN_HANDLE_INVALID;
  }
  else{
    _err_code = sd_ble_gap_adv_stop();
    NRF_LOG_INFO("{%s} %d\n", (uint32_t)__func__, _err_code);
  }

  /*sd_softdevice_disable();*/
  return _err_code != NRF_SUCCESS ? IC_ERROR : IC_SUCCESS;
}

ic_return_val_e ic_bluetooth_enable(void){
  if(m_module_initialized == false) return IC_NOT_INIALIZED;
  m_ble_power_down = false;
  pm_peers_delete();
  ble_advertising_start(BLE_ADV_MODE_FAST);

  return NRF_SUCCESS;
}
