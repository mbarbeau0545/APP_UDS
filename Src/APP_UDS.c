/** @file APP_UDS.c @brief APPSYS-managed UDS diagnostics over the FMKFDCAN public interface. */
#include "APP_UDS.h"
#include "APP_CTRL/APP_SYS/Src/APP_SYS.h"
#include "APP_CFG/ConfigFiles/APPUDS_ConfigPrivate.h"
#include "APP_CFG/ConfigSpecific/AppUdsConfigSpecific/APPUDS_ConfigSpecific.h"
#include "FMK_HAL/FMK_CAN/Src/FMK_FDCAN.h"
#include "FMK_HAL/FMK_CPU/Src/FMK_CPU.h"
#include "Library/ISO_TP/Src/LIBIsoTp.h"
#include "Library/FIFO/Src/LIBFifo.h"
#include <string.h>

/// @brief Copied receive descriptor, independent of transient framework buffers.
typedef struct
{
    t_uint8 data_au8[8];    ///< Internal frame bytes.
    t_uint8 size_u8;        ///< Actual received DLC.
} t_sAPPUDS_CanFrame;

/// @brief Configured module lifecycle, visible even when the generated profile is disabled.
static volatile t_eCyclicModState g_AppUdsState_e = STATE_CYCLIC_BUSY;

/// @brief Preserve the physical subscription across module suspension and reconfiguration.
static t_bool g_SubscriptionRegistered_b;
/// @brief Library-owned ISO-TP connection.
static t_sLIBISOTP_Context g_Transport_s;
/// @brief Bounded RX payload storage.
static t_uint8 g_Rx_au8[APPUDS_RX_BUFFER_SIZE];
/// @brief Bounded TX payload storage.
static t_uint8 g_Tx_au8[APPUDS_TX_BUFFER_SIZE];
/// @brief Owned frame descriptors, released in FIFO order.
static t_sAPPUDS_CanFrame g_Frames_as[APPUDS_RX_QUEUE_SIZE];
/// @brief FIFO managed exclusively through LIBFIFO APIs.
static t_sLIBFIFO_Core g_RxFifo_s;
/// @brief Current monotone tick.
static t_uint32 g_Now_u32;
/// @brief Queue overrun indication; a lost CF must invalidate reassembly.
static volatile t_bool g_Overflow_b;

/// @brief The APPSYS main-context FastTask has been registered for this boot.
static t_bool g_FastTaskRegistered_b;
/// @brief Activation shared by the CAN callback and serialized main-context service.
static volatile t_bool g_FastTaskEnabled_b;
/// @brief Tick of the last operational step, preventing double dispatch at a slow-cycle boundary.
static t_uint32 g_LastWorkTick_u32;
/// @brief Whether a prior operational tick exists, including when the first tick is zero.
static t_bool g_WorkTickValid_b;

// Local functions - Prototypes
/** @brief Copy a short physical CAN frame in callback context.
 * @param[in] f_Node_e Node. @param[in] f_RxItem_s Borrowed framework bytes.
 * @param[in] f_NodeStatus_e Framework health. No pointer is retained.
 */
static void s_APPUDS_Receive(t_eFMKFDCAN_NodeList f_Node_e, t_sFMKFDCAN_RxItemEvent f_RxItem_s,
                             t_eFMKFDCAN_NodeStatus f_NodeStatus_e);
/** @brief Submit eight copied bytes through the public FDCAN API.
 * @param[in] f_Context_pv Unused. @param[in] f_Data_pu8 Borrowed bytes; copied by FMKFDCAN.
 * @retval RC_OK Queued. @retval RC_WARNING_BUSY Retry later.
 */
static t_eReturnCode s_APPUDS_SendFrame(void * f_Context_pv, const t_uint8 * f_Data_pu8);
/** @brief Copy an ISO-TP PDU to the application server.
 * @param[in] f_Context_pv Unused. @param[in] f_Data_pu8 Borrowed PDU. @param[in] f_Size_u16 Exact length.
 * @retval RC_OK Copied. @retval RC_WARNING_BUSY Active request preserved.
 */
static t_eReturnCode s_APPUDS_ReceivePayload(void * f_Context_pv, const t_uint8 * f_Data_pu8,
                                           t_uint16 f_Size_u16);
/** @brief Subscribe to the physical request ID after framework-owned node activation.
 * @retval RC_OK Subscribed. @retval RC_WARNING_PENDING Framework startup still pending.
 * @retval RC_ERROR_INSTANCE_NOT_INITIALIZED FMKFDCAN reported a startup failure.
 */
static t_eReturnCode s_APPUDS_Configure(void);
/** @brief Drain a bounded frame batch and progress transport/server in cyclic context.
 * @retval RC_OK Progressed. @retval RC_WARNING_PENDING Server active.
 */
static t_eReturnCode s_APPUDS_Run(void);
/** @brief Wait for transport startup while retaining PREOPE.
 * @retval RC_OK Transport subscribed and server ready.
 * @retval RC_WARNING_PENDING Framework startup in progress.
 * @retval RC_ERROR_INSTANCE_NOT_INITIALIZED Framework startup failed.
 */
static t_eReturnCode s_APPUDS_PreOperational(void);
/** @brief Hold an adapter failure without touching hardware. @retval RC_ERROR_WRONG_STATE Held. */
static t_eReturnCode s_APPUDS_Error(void);
/** @brief Hold a suspended port without touching queued frames. @retval RC_WARNING_NO_OPERATION Suspended. */
static t_eReturnCode s_APPUDS_Hold(void);
/** @brief Initialize owned FIFO and transport storage. @retval RC_OK Armed. */
static t_eReturnCode s_APPUDS_Arm(void);

/** @brief Run a timer-notified diagnostic step only from serialized APPSYS main context. */
static void s_APPUDS_FastTask(void);
/**
 * @brief Progress at most one operational step per observed tick and update FastTask activation.
 * @retval RC_OK Progressed. @retval RC_WARNING_NO_OPERATION Current tick already serviced.
 * @retval RC_ERROR_LIMIT_REACHED RX overrun; lifecycle switch enters ERROR.
 */
static t_eReturnCode s_APPUDS_Operational(void);
/**
 * @brief Activate fast service while RX, ISO-TP or server work remains outstanding.
 * @note Queue inspection and mask publication share a short critical section to avoid a lost receive wakeup.
 * @retval RC_OK Activation matches the current work. Scheduler and query errors are propagated.
 */
static t_eReturnCode s_APPUDS_UpdateFastTask(void);
/**
 * @brief Change the registered task activation from main or receive callback context.
 * @param[in] f_Enabled_b TRUE requests 1 ms main-context service; FALSE clears pending notifications.
 * @retval RC_OK Applied or already in the requested state. Scheduler failures are propagated.
 */
static t_eReturnCode s_APPUDS_SetFastTask(t_bool f_Enabled_b);

/*********************************
 * APPUDS_Init
 *********************************/
t_eReturnCode APPUDS_Init(void)
{
    t_eReturnCode Ret_e = RC_OK;

    //---- 1- Disabled profiles are valid registered modules with a suspended lifecycle ----//
    g_AppUdsState_e = STATE_CYCLIC_BUSY;
    g_FastTaskRegistered_b = FALSE;
    g_FastTaskEnabled_b = FALSE;
    g_WorkTickValid_b = FALSE;
    if(APPUDS_ENABLED != 0U)
    {
        g_AppUdsState_e = STATE_CYCLIC_CFG;
        g_SubscriptionRegistered_b = FALSE;
        Ret_e = s_APPUDS_Arm();
        if(Ret_e == RC_OK)
        {
            //---- 2- Register the fast callback without enabling it during idle startup ----//
            Ret_e = APPSYS_AddMainFastTask(APPSYS_MODULE_APP_UDS, s_APPUDS_FastTask);
            if(Ret_e == RC_OK)
            {
                g_FastTaskRegistered_b = TRUE;
            }
        }
        if(Ret_e != RC_OK)
        {
            g_AppUdsState_e = STATE_CYCLIC_ERROR;
        }
    }

    return Ret_e;
}

/*********************************
 * APPUDS_Cyclic
 *********************************/
t_eReturnCode APPUDS_Cyclic(void)
{
    t_eReturnCode Ret_e = RC_WARNING_NO_OPERATION;

    //---- 1- Centralize lifecycle transitions and defer all protocol work to cyclic context ----//
    if(APPUDS_ENABLED != 0U)
    {
        FMKCPU_GetTick(&g_Now_u32);
        switch(g_AppUdsState_e)
        {
            case STATE_CYCLIC_CFG:
            {
                Ret_e = s_APPUDS_Configure();
                if(Ret_e == RC_OK)
                {
                    g_AppUdsState_e = STATE_CYCLIC_OPE;
                }
                else if(Ret_e < RC_OK)
                {
                    g_AppUdsState_e = STATE_CYCLIC_ERROR;
                }
                break;
            }
            case STATE_CYCLIC_PREOPE:
            {
                Ret_e = s_APPUDS_PreOperational();
                if(Ret_e == RC_OK)
                {
                    g_AppUdsState_e = STATE_CYCLIC_OPE;
                }
                else if(Ret_e < RC_OK)
                {
                    g_AppUdsState_e = STATE_CYCLIC_ERROR;
                }
                break;
            }
            case STATE_CYCLIC_OPE:
            {
                Ret_e = s_APPUDS_Operational();
                if(Ret_e < RC_OK)
                {
                    g_AppUdsState_e = STATE_CYCLIC_ERROR;
                }
                break;
            }
            case STATE_CYCLIC_ERROR:
            {
                Ret_e = s_APPUDS_Error();
                break;
            }
            default:
            {
                Ret_e = s_APPUDS_Hold();
                break;
            }
        }
    }

    //---- 2- Suspended, failed and startup states never retain an active fast callback ----//
    if((g_FastTaskRegistered_b == TRUE) && (g_AppUdsState_e != STATE_CYCLIC_OPE))
    {
        t_eReturnCode schedulerRet_e = s_APPUDS_SetFastTask(FALSE);
        if((Ret_e >= RC_OK) && (schedulerRet_e < RC_OK))
        {
            Ret_e = schedulerRet_e;
        }
    }

    return Ret_e;
}

/*********************************
 * APPUDS_GetState
 *********************************/
t_eReturnCode APPUDS_GetState(t_eCyclicModState * f_State_pe)
{
    t_eReturnCode Ret_e = RC_OK;

    //---- 1- Report the configured port lifecycle without accessing hardware ----//
    if(f_State_pe == NULL)
    {
        Ret_e = RC_ERROR_PTR_NULL;
    }
    else
    {
        *f_State_pe = g_AppUdsState_e;
    }

    return Ret_e;
}

/*********************************
 * APPUDS_SetState
 *********************************/
t_eReturnCode APPUDS_SetState(t_eCyclicModState f_State_e)
{
    t_eReturnCode Ret_e = RC_OK;

    //---- 1- Validate lifecycle requests before changing transport or server state ----//
    if((t_uint32)f_State_e > (t_uint32)STATE_CYCLIC_ERROR)
    {
        Ret_e = RC_ERROR_PARAM_INVALID;
    }
    else
    {
        if(APPUDS_ENABLED != 0U)
        {
            if((f_State_e == STATE_CYCLIC_OPE) && (g_SubscriptionRegistered_b == FALSE))
            {
                Ret_e = RC_WARNING_PENDING;
            }
            else
            {
                t_eCyclicModState serverState_e = f_State_e;
                if(f_State_e == STATE_CYCLIC_PREOPE)
                {
                    serverState_e = STATE_CYCLIC_CFG;
                }
                //---- 2- Revoke active server authorization when operational processing is suspended ----//
                Ret_e = APPUDS_ServerSetState(serverState_e);
                if(Ret_e == RC_OK)
                {
                    g_AppUdsState_e = f_State_e;
                }
            }
        }
        else
        {
            //---- 2- A disabled profile cannot be activated through a state request ----//
            g_AppUdsState_e = STATE_CYCLIC_BUSY;
            if(f_State_e == STATE_CYCLIC_ERROR)
            {
                g_AppUdsState_e = STATE_CYCLIC_ERROR;
            }
        }
    }

    //---- 3- Cancel deferred fast notifications immediately when the lifecycle leaves OPE ----//
    if((Ret_e == RC_OK) && (g_FastTaskRegistered_b == TRUE) && (g_AppUdsState_e != STATE_CYCLIC_OPE))
    {
        Ret_e = s_APPUDS_SetFastTask(FALSE);
    }

    return Ret_e;
}

/*********************************
 * APPUDS_SendPayload_PRIVATE_
 *********************************/
t_eReturnCode APPUDS_SendPayload_PRIVATE_(const t_uint8 * f_Data_pu8, t_uint16 f_Size_u16)
{
    t_eReturnCode Ret_e = RC_ERROR_NOT_ALLOWED;

    //---- 1- Copy outgoing payload before releasing the server response reservation ----//
    if((APPUDS_ENABLED != 0U) && (g_AppUdsState_e == STATE_CYCLIC_OPE))
    {
        Ret_e = LIBISOTP_Send(&g_Transport_s, f_Data_pu8, f_Size_u16, g_Now_u32);
    }

    return Ret_e;
}

/*********************************
 * s_APPUDS_Arm
 *********************************/
static t_eReturnCode s_APPUDS_Arm(void)
{
    //---- 1- Bind persistent owned buffers and arm without configuring CAN ----//
    t_sLIBFIFO_Cfg fifo_s = {g_Frames_as, sizeof(g_Frames_as[0]), APPUDS_RX_QUEUE_SIZE};
    t_eReturnCode Ret_e = LIBFIFO_Create(&g_RxFifo_s, fifo_s);
    if(Ret_e == RC_OK)
    {
        t_sLIBISOTP_Config config_s = {g_Rx_au8, g_Tx_au8, sizeof(g_Rx_au8), sizeof(g_Tx_au8),
                                       APPUDS_ISOTP_TIMEOUT_MS, 8U, 0U, 3U,
                                       s_APPUDS_SendFrame, s_APPUDS_ReceivePayload, NULL};
        Ret_e = LIBISOTP_Init(&g_Transport_s, &config_s);
    }
    if(Ret_e == RC_OK)
    {
        Ret_e = APPUDS_ServerInit(&c_APPUDS_Config_s);
    }
    g_Overflow_b = FALSE;

    return Ret_e;
}

/*********************************
 * s_APPUDS_Configure
 *********************************/
static t_eReturnCode s_APPUDS_Configure(void)
{
    t_eCyclicModState state_e = STATE_CYCLIC_CFG;
    t_eReturnCode Ret_e = FMKFDCAN_GetState(&state_e);

    //---- 1- Wait for framework-owned driver startup and activation ----//
    if(Ret_e == RC_OK)
    {
        if(state_e == STATE_CYCLIC_ERROR)
        {
            Ret_e = RC_ERROR_INSTANCE_NOT_INITIALIZED;
        }
        else if(state_e != STATE_CYCLIC_OPE)
        {
            Ret_e = RC_WARNING_PENDING;
        }
        else
        {
            //---- 2- Subscribe only to the physical ID after the configured node is ready ----//
            t_sFMKFDCAN_RxItemEventCfg subscription_s = {0};
            subscription_s.ItemId_s.Identifier_u32 = APPUDS_RX_CAN_ID;
            subscription_s.ItemId_s.IdType_e = FMKFDCAN_IDTYPE_STANDARD;
            subscription_s.ItemId_s.FramePurpose_e = FMKFDCAN_FRAME_PURPOSE_DATA;
            subscription_s.maskId_u32 = 0x7FFU;
#if APPUDS_CAN_ID_FORMAT == 29U
            subscription_s.ItemId_s.IdType_e = FMKFDCAN_IDTYPE_EXTENDED;
            subscription_s.maskId_u32 = 0x1FFFFFFFUL;
#endif
            subscription_s.Dlc_e = FMKFDCAN_DLC_8;
            subscription_s.callback_cb = s_APPUDS_Receive;
            if(g_SubscriptionRegistered_b == FALSE)
            {
                Ret_e = FMKFDCAN_ConfigureRxItemEvent(APPUDS_CAN_NODE, subscription_s);
                if(Ret_e == RC_OK)
                {
                    g_SubscriptionRegistered_b = TRUE;
                }
            }
            if(Ret_e == RC_OK)
            {
                Ret_e = APPUDS_ServerCyclic(g_Now_u32);
            }
        }
    }

    return Ret_e;
}

/*********************************
 * s_APPUDS_Run
 *********************************/
static t_eReturnCode s_APPUDS_Run(void)
{
    t_eReturnCode Ret_e = RC_OK;

    //---- 1- Pop only a bounded batch while protecting each short FIFO operation ----//
    for(t_uint8 idx_u8 = 0U; idx_u8 < APPUDS_RX_QUEUE_SIZE; idx_u8++)
    {
        t_sAPPUDS_CanFrame frame_s = {0};
        t_uint32 previous_u32 = APPUDS_EnterQueueCritical_PRIVATE_();
        Ret_e = LIBFIFO_Pop(&g_RxFifo_s, &frame_s, sizeof(frame_s));
        APPUDS_ExitQueueCritical_PRIVATE_(previous_u32);
        if(Ret_e == RC_OK)
        {
            Ret_e = LIBISOTP_OnFrame(&g_Transport_s, frame_s.data_au8, frame_s.size_u8, g_Now_u32);
            //---- 2- Emit receiver flow control before consuming a following CF ----//
            Ret_e = LIBISOTP_Cyclic(&g_Transport_s, g_Now_u32);
        }
    }
    if(g_Overflow_b == TRUE)
    {
        //---- 3- Fail closed after lost frames; reinitialization requires explicit intervention ----//
        Ret_e = APPUDS_ServerSetState(STATE_CYCLIC_ERROR);
        Ret_e = RC_ERROR_LIMIT_REACHED;
    }
    else
    {
        Ret_e = LIBISOTP_Cyclic(&g_Transport_s, g_Now_u32);
        // Transport protocol failures abort the transfer; subsequent diagnostic requests remain possible.
        if(Ret_e != RC_WARNING_BUSY)
        {
            Ret_e = APPUDS_ServerCyclic(g_Now_u32);
        }
    }

    return Ret_e;
}

/*********************************
 * s_APPUDS_Error
 *********************************/
static t_eReturnCode s_APPUDS_Error(void)
{
    //---- 1- Preserve the failed adapter without performing hardware operations ----//
    t_eReturnCode Ret_e = RC_ERROR_WRONG_STATE;

    return Ret_e;
}

/*********************************
 * s_APPUDS_Hold
 *********************************/
static t_eReturnCode s_APPUDS_Hold(void)
{
    //---- 1- Preserve suspended protocol ownership without invoking any backend ----//
    t_eReturnCode Ret_e = RC_WARNING_NO_OPERATION;

    return Ret_e;
}

/*********************************
 * s_APPUDS_Receive
 *********************************/
static void s_APPUDS_Receive(t_eFMKFDCAN_NodeList f_Node_e, t_sFMKFDCAN_RxItemEvent f_RxItem_s,
                             t_eFMKFDCAN_NodeStatus f_NodeStatus_e)
{
    t_eFMKFDCAN_IdentifierType idType_e = FMKFDCAN_IDTYPE_STANDARD;
#if APPUDS_CAN_ID_FORMAT == 29U
    idType_e = FMKFDCAN_IDTYPE_EXTENDED;
#endif

    //---- 1- Copy validated physical frames before the framework releases its buffer ----//
    if((g_AppUdsState_e == STATE_CYCLIC_OPE) && (f_Node_e == APPUDS_CAN_NODE)
     && (f_NodeStatus_e == FMKFDCAN_NODE_STATE_OK)
     && (f_RxItem_s.ItemId_s.Identifier_u32 == APPUDS_RX_CAN_ID)
     && (f_RxItem_s.ItemId_s.IdType_e == idType_e)
     && (f_RxItem_s.ItemId_s.FramePurpose_e == FMKFDCAN_FRAME_PURPOSE_DATA)
     && (f_RxItem_s.CanMsg_s.data_pu8 != NULL)
     && (f_RxItem_s.CanMsg_s.Dlc_e > 0) && (f_RxItem_s.CanMsg_s.Dlc_e <= FMKFDCAN_DLC_8))
    {
        t_sAPPUDS_CanFrame frame_s = {0};
        frame_s.size_u8 = (t_uint8)f_RxItem_s.CanMsg_s.Dlc_e;
        memcpy(frame_s.data_au8, f_RxItem_s.CanMsg_s.data_pu8, frame_s.size_u8);
        t_uint32 previous_u32 = APPUDS_EnterQueueCritical_PRIVATE_();
        t_eReturnCode Ret_e = LIBFIFO_Push(&g_RxFifo_s, &frame_s, sizeof(frame_s));
        if(Ret_e != RC_OK)
        {
            g_Overflow_b = TRUE;
        }
        //---- 2- Wake the registered 1 ms task without executing any protocol or business work in the IRQ ----//
        Ret_e = s_APPUDS_SetFastTask(TRUE);
        if(Ret_e != RC_OK)
        {
            g_Overflow_b = TRUE;
        }
        APPUDS_ExitQueueCritical_PRIVATE_(previous_u32);
    }

    return;
}

/*********************************
 * s_APPUDS_SendFrame
 *********************************/
static t_eReturnCode s_APPUDS_SendFrame(void * f_Context_pv, const t_uint8 * f_Data_pu8)
{
    t_sFMKFDCAN_TxItem item_s = {0};
    (void)f_Context_pv;

    //---- 1- Submit a classical CAN frame through the public copying API ----//
    item_s.ItemId_s.Identifier_u32 = APPUDS_TX_CAN_ID;
    item_s.ItemId_s.IdType_e = FMKFDCAN_IDTYPE_STANDARD;
#if APPUDS_CAN_ID_FORMAT == 29U
    item_s.ItemId_s.IdType_e = FMKFDCAN_IDTYPE_EXTENDED;
#endif
    item_s.ItemId_s.FramePurpose_e = FMKFDCAN_FRAME_PURPOSE_DATA;
    item_s.CanMsg_s.Dlc_e = FMKFDCAN_DLC_8;
    item_s.CanMsg_s.Direction_e = FMKFDCAN_NODE_DIRECTION_TX;
    item_s.CanMsg_s.data_pu8 = (t_uint8 *)f_Data_pu8;
    item_s.frameFormat_e = FMKFDCAN_FRAME_FORMAT_CLASSIC;
    item_s.BitRate_e = FMKFDCAN_BITRATE_SWITCH_OFF;
    t_eReturnCode Ret_e = FMKFDCAN_SendTxItem(APPUDS_CAN_NODE, item_s);
    if(Ret_e > RC_OK)
    {
        Ret_e = RC_WARNING_BUSY;
    }

    return Ret_e;
}

/*********************************
 * s_APPUDS_ReceivePayload
 *********************************/
static t_eReturnCode s_APPUDS_ReceivePayload(void * f_Context_pv, const t_uint8 * f_Data_pu8,
                                           t_uint16 f_Size_u16)
{
    //---- 1- Forward trusted physical addressing metadata with a copying payload API ----//
    (void)f_Context_pv;
    t_eReturnCode Ret_e = APPUDS_OnPayload(f_Data_pu8, f_Size_u16, FALSE);

    return Ret_e;
}

/*********************************
 * s_APPUDS_PreOperational
 *********************************/
static t_eReturnCode s_APPUDS_PreOperational(void)
{
    //---- 1- Finish the same bounded startup contract when resuming from PREOPE ----//
    t_eReturnCode Ret_e = s_APPUDS_Configure();

    return Ret_e;
}

/*********************************
 * s_APPUDS_FastTask
 *********************************/
static void s_APPUDS_FastTask(void)
{
    //---- 1- Reuse the public lifecycle switch from serialized APPSYS main context ----//
    t_eReturnCode Ret_e = APPUDS_Cyclic();
    (void)Ret_e;

    return;
}

/*********************************
 * s_APPUDS_Operational
 *********************************/
static t_eReturnCode s_APPUDS_Operational(void)
{
    t_eReturnCode Ret_e = RC_WARNING_NO_OPERATION;

    //---- 1- Coalesce the regular lifecycle call with the fast callback at the same tick ----//
    if((g_WorkTickValid_b == FALSE) || (g_LastWorkTick_u32 != g_Now_u32))
    {
        g_LastWorkTick_u32 = g_Now_u32;
        g_WorkTickValid_b = TRUE;
        Ret_e = s_APPUDS_Run();
    }
    if(Ret_e >= RC_OK)
    {
        //---- 2- Keep 1 ms service only while an exchange, flow-control wait or backend step is pending ----//
        t_eReturnCode schedulerRet_e = s_APPUDS_UpdateFastTask();
        if(schedulerRet_e < RC_OK)
        {
            Ret_e = schedulerRet_e;
        }
    }

    return Ret_e;
}

/*********************************
 * s_APPUDS_SetFastTask
 *********************************/
static t_eReturnCode s_APPUDS_SetFastTask(t_bool f_Enabled_b)
{
    t_eReturnCode Ret_e = RC_OK;

    //---- 1- Serialize receive wakeups and idle deactivation without holding interrupts during business work ----//
    t_uint32 key_u32 = APPUDS_EnterQueueCritical_PRIVATE_();
    if((g_FastTaskRegistered_b == TRUE) && (g_FastTaskEnabled_b != f_Enabled_b))
    {
        t_eAPPSYS_FastTaskState state_e = APPSYS_FAST_TASK_DISABLE;
        if(f_Enabled_b == TRUE)
        {
            state_e = APPSYS_FAST_TASK_ENABLE;
        }
        Ret_e = APPSYS_SetFastTaskState(APPSYS_MODULE_APP_UDS, state_e);
        if(Ret_e == RC_OK)
        {
            g_FastTaskEnabled_b = f_Enabled_b;
        }
    }
    APPUDS_ExitQueueCritical_PRIVATE_(key_u32);

    return Ret_e;
}

/*********************************
 * s_APPUDS_UpdateFastTask
 *********************************/
static t_eReturnCode s_APPUDS_UpdateFastTask(void)
{
    t_bool transportActive_b = FALSE;
    t_bool serverPending_b = FALSE;

    //---- 1- Query protocol and server ownership through their public APIs in main context ----//
    t_eReturnCode Ret_e = LIBISOTP_GetActivity(&g_Transport_s, &transportActive_b);
    if(Ret_e == RC_OK)
    {
        Ret_e = APPUDS_ServerHasPending(&serverPending_b);
    }
    if(Ret_e == RC_OK)
    {
        //---- 2- Inspect the receive queue and publish activation atomically to preserve racing wakeups ----//
        t_uint32 key_u32 = APPUDS_EnterQueueCritical_PRIVATE_();
        t_uint16 count_u16 = 0U;
        Ret_e = LIBFIFO_GetCount(&g_RxFifo_s, &count_u16);
        if(Ret_e == RC_OK)
        {
            t_bool active_b = (count_u16 != 0U) || (g_Overflow_b == TRUE)
                              || (transportActive_b == TRUE) || (serverPending_b == TRUE);
            Ret_e = s_APPUDS_SetFastTask(active_b);
        }
        APPUDS_ExitQueueCritical_PRIVATE_(key_u32);
    }

    return Ret_e;
}
