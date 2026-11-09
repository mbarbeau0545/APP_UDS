/** @file APPUDS_FlashMock.c @brief Bounded RAM staging backend; no image can activate hardware. */
#include "APPUDS_FlashPorts.h"
#include <string.h>

/// @brief Maximum simulated image size; independent of real Flash geometry.
#define APPUDS_MOCK_IMAGE_SIZE 4096U
/// @brief Staging RAM owned by the mock port.
static t_uint8 g_Image_au8[APPUDS_MOCK_IMAGE_SIZE];
/// @brief Expected image size.
static t_uint32 g_Size_u32;
/// @brief Committed sequential bytes.
static t_uint32 g_Committed_u32;
/// @brief Transfer was explicitly closed.
static t_bool g_Closed_b;
/// @brief Test verifier; null denies image verification.
static t_cbAPPUDS_MockVerifier * g_Verifier_pcb;

// Local functions - Prototypes
/** @brief Prepare mock staging. @param[in] f_First_b Start. @param[in] f_Offset_u32 Logical region.
 * @param[in] f_Data_pu8 Unused. @param[in] f_Size_u32 Image size.
 * @retval RC_OK Prepared. @retval RC_ERROR_PARAM_INVALID Invalid region or capacity.
 */
static t_eReturnCode s_APPUDS_MockBegin(t_bool f_First_b, t_uint32 f_Offset_u32,
                                      const t_uint8 * f_Data_pu8, t_uint32 f_Size_u32);
/** @brief Copy one sequential block. No input is retained.
 * @param[in] f_First_b Start. @param[in] f_Offset_u32 Image offset.
 * @param[in] f_Data_pu8 Borrowed block. @param[in] f_Size_u32 Block size.
 * @retval RC_OK Copied. @retval RC_ERROR_PARAM_INVALID Nonsequential or oversized block.
 */
static t_eReturnCode s_APPUDS_MockWrite(t_bool f_First_b, t_uint32 f_Offset_u32,
                                      const t_uint8 * f_Data_pu8, t_uint32 f_Size_u32);
/** @brief Close a complete mock image without activation.
 * @param[in] f_First_b Start. @param[in] f_Offset_u32 Unused. @param[in] f_Data_pu8 Unused.
 * @param[in] f_Size_u32 Unused. @retval RC_OK Closed. @retval RC_ERROR_WRONG_STATE Incomplete image.
 */
static t_eReturnCode s_APPUDS_MockFinish(t_bool f_First_b, t_uint32 f_Offset_u32,
                                       const t_uint8 * f_Data_pu8, t_uint32 f_Size_u32);
/** @brief Delegate verification to an explicit test verifier; null denies.
 * @param[in] f_First_b Start. @param[in] f_Offset_u32 Unused.
 * @param[in] f_Data_pu8 Borrowed manifest. @param[in] f_Size_u32 Manifest size.
 * @retval RC_OK Test verifier accepted. @retval RC_ERROR_NOT_ALLOWED No verifier or incomplete image.
 */
static t_eReturnCode s_APPUDS_MockVerify(t_bool f_First_b, t_uint32 f_Offset_u32,
                                       const t_uint8 * f_Data_pu8, t_uint32 f_Size_u32);

/// @brief Test-only logical staging contract; activation is always denied.
const t_sAPPUDS_FlashPort c_APPUDS_MockFlash_s =
{
    1U, APPUDS_MOCK_IMAGE_SIZE, APPUDS_MAX_WRITE_PAYLOAD + 2U, TRUE,
    s_APPUDS_MockBegin, s_APPUDS_MockWrite, s_APPUDS_MockFinish, s_APPUDS_MockVerify, APPUDS_FlashDenied
};

/*********************************
 * APPUDS_MockInit
 *********************************/
t_eReturnCode APPUDS_MockInit(t_cbAPPUDS_MockVerifier * f_Verifier_pcb)
{
    //---- 1- Drop all simulated staging metadata on reset or power-loss injection ----//
    t_eReturnCode Ret_e = RC_OK;
    memset(g_Image_au8, 0xFF, sizeof(g_Image_au8));
    g_Size_u32 = 0U;
    g_Committed_u32 = 0U;
    g_Closed_b = FALSE;
    g_Verifier_pcb = f_Verifier_pcb;

    return Ret_e;
}

/*********************************
 * APPUDS_MockRead
 *********************************/
t_eReturnCode APPUDS_MockRead(t_uint32 f_Offset_u32, t_uint8 * f_Data_pu8, t_uint16 f_Size_u16)
{
    t_eReturnCode Ret_e = RC_OK;

    //---- 1- Bound readback to already committed mock bytes ----//
    if((f_Data_pu8 == NULL) || (f_Offset_u32 > g_Committed_u32)
     || (f_Size_u16 > g_Committed_u32 - f_Offset_u32))
    {
        Ret_e = RC_ERROR_PARAM_INVALID;
    }
    else
    {
        memcpy(f_Data_pu8, &g_Image_au8[f_Offset_u32], f_Size_u16);
    }

    return Ret_e;
}

/*********************************
 * APPUDS_FlashDenied
 *********************************/
t_eReturnCode APPUDS_FlashDenied(t_bool f_First_b, t_uint32 f_Offset_u32,
                                 const t_uint8 * f_Data_pu8, t_uint32 f_Size_u32)
{
    //---- 1- Reject hardware access regardless of wire or application input ----//
    t_eReturnCode Ret_e = RC_ERROR_NOT_ALLOWED;
    (void)f_First_b;
    (void)f_Offset_u32;
    (void)f_Data_pu8;
    (void)f_Size_u32;

    return Ret_e;
}

/*********************************
 * s_APPUDS_MockBegin
 *********************************/
static t_eReturnCode s_APPUDS_MockBegin(t_bool f_First_b, t_uint32 f_Offset_u32,
                                      const t_uint8 * f_Data_pu8, t_uint32 f_Size_u32)
{
    t_eReturnCode Ret_e = RC_OK;
    (void)f_Data_pu8;

    //---- 1- Prepare only the bounded simulated logical region ----//
    if((f_Offset_u32 != 1U) || (f_Size_u32 == 0U) || (f_Size_u32 > sizeof(g_Image_au8)))
    {
        Ret_e = RC_ERROR_PARAM_INVALID;
    }
    else if(f_First_b == TRUE)
    {
        memset(g_Image_au8, 0xFF, sizeof(g_Image_au8));
        g_Size_u32 = f_Size_u32;
        g_Committed_u32 = 0U;
        g_Closed_b = FALSE;
    }

    return Ret_e;
}

/*********************************
 * s_APPUDS_MockWrite
 *********************************/
static t_eReturnCode s_APPUDS_MockWrite(t_bool f_First_b, t_uint32 f_Offset_u32,
                                      const t_uint8 * f_Data_pu8, t_uint32 f_Size_u32)
{
    t_eReturnCode Ret_e = RC_OK;
    (void)f_First_b;

    //---- 1- Commit only an in-range sequential block into RAM ----//
    if((f_Data_pu8 == NULL) || (g_Closed_b == TRUE) || (f_Offset_u32 != g_Committed_u32)
     || (f_Size_u32 == 0U) || (f_Size_u32 > g_Size_u32 - g_Committed_u32))
    {
        Ret_e = RC_ERROR_PARAM_INVALID;
    }
    else
    {
        memcpy(&g_Image_au8[g_Committed_u32], f_Data_pu8, f_Size_u32);
        g_Committed_u32 += f_Size_u32;
    }

    return Ret_e;
}

/*********************************
 * s_APPUDS_MockFinish
 *********************************/
static t_eReturnCode s_APPUDS_MockFinish(t_bool f_First_b, t_uint32 f_Offset_u32,
                                       const t_uint8 * f_Data_pu8, t_uint32 f_Size_u32)
{
    t_eReturnCode Ret_e = RC_OK;
    (void)f_First_b;
    (void)f_Offset_u32;
    (void)f_Data_pu8;
    (void)f_Size_u32;

    //---- 1- Mark only a complete nonempty simulated image as closed ----//
    if((g_Size_u32 == 0U) || (g_Committed_u32 != g_Size_u32))
    {
        Ret_e = RC_ERROR_WRONG_STATE;
    }
    else
    {
        g_Closed_b = TRUE;
    }

    return Ret_e;
}

/*********************************
 * s_APPUDS_MockVerify
 *********************************/
static t_eReturnCode s_APPUDS_MockVerify(t_bool f_First_b, t_uint32 f_Offset_u32,
                                       const t_uint8 * f_Data_pu8, t_uint32 f_Size_u32)
{
    t_eReturnCode Ret_e = RC_ERROR_NOT_ALLOWED;
    (void)f_First_b;
    (void)f_Offset_u32;

    //---- 1- Require a closed image and an explicit test verifier; no built-in unlock exists ----//
    if((g_Closed_b == TRUE) && (g_Verifier_pcb != NULL) && (f_Data_pu8 != NULL) && (f_Size_u32 != 0U))
    {
        Ret_e = g_Verifier_pcb(g_Image_au8, g_Size_u32, f_Data_pu8, f_Size_u32);
    }

    return Ret_e;
}
