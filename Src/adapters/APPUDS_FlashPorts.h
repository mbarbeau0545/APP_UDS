/** @file APPUDS_FlashPorts.h @brief Simulation and disabled target-specific bootloader boundaries. */
#ifndef APPUDS_FLASHPORTS_H_INCLUDED
#define APPUDS_FLASHPORTS_H_INCLUDED
#include "../APP_UDS.h"

/// @brief RAM-only test port; never linked to hardware activation.
extern const t_sAPPUDS_FlashPort c_APPUDS_MockFlash_s;
/// @brief H753 port disabled until partition, signature and recovery approval.
extern const t_sAPPUDS_FlashPort c_APPUDS_FlashPortH753_s;
/// @brief G474 port disabled until independent partition, signature and recovery approval.
extern const t_sAPPUDS_FlashPort c_APPUDS_FlashPortG474_s;

/**
 * @brief Verify a simulated image against a test manifest.
 * @note Test-only synchronous callback. No pointer may be retained. This is not production signature validation.
 * @param[in] f_Image_pu8 Borrowed mock image. @param[in] f_ImageSize_u32 Exact image size.
 * @param[in] f_Manifest_pu8 Borrowed test manifest. @param[in] f_ManifestSize_u32 Exact manifest size.
 * @retval RC_OK Test verifier accepted image and manifest.
 * @retval RC_ERROR_WRONG_RESULT Mock signature or digest mismatch.
 */
typedef t_eReturnCode t_cbAPPUDS_MockVerifier(const t_uint8 * f_Image_pu8, t_uint32 f_ImageSize_u32,
                                            const t_uint8 * f_Manifest_pu8, t_uint32 f_ManifestSize_u32);
/** @brief Reset mock state and install a test verifier; no hardware is accessed.
 * @param[in] f_Verifier_pcb Persistent callback, null denies all verification.
 * @retval RC_OK Reset.
 */
t_eReturnCode APPUDS_MockInit(t_cbAPPUDS_MockVerifier * f_Verifier_pcb);
/** @brief Read back committed mock bytes for fault-injection tests.
 * @param[in] f_Offset_u32 Offset. @param[out] f_Data_pu8 Caller destination.
 * @param[in] f_Size_u16 Exact count. No pointer is retained.
 * @retval RC_OK Copied. @retval RC_ERROR_PARAM_INVALID Invalid range.
 */
t_eReturnCode APPUDS_MockRead(t_uint32 f_Offset_u32, t_uint8 * f_Data_pu8, t_uint16 f_Size_u16);
/** @brief Reject every real Flash operation until a protected board port is approved.
 * @param[in] f_First_b Start flag. @param[in] f_Offset_u32 Logical value.
 * @param[in] f_Data_pu8 Borrowed bytes, never dereferenced or retained. @param[in] f_Size_u32 Byte count.
 * @retval RC_ERROR_NOT_ALLOWED Hardware programming is disabled.
 */
t_eReturnCode APPUDS_FlashDenied(t_bool f_First_b, t_uint32 f_Offset_u32,
                                 const t_uint8 * f_Data_pu8, t_uint32 f_Size_u32);
#endif
