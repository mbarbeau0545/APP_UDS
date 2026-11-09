/** @file APPUDS_Programming.h @brief Logical download services; no physical Flash access. */
#ifndef APPUDS_PROGRAMMING_H_INCLUDED
#define APPUDS_PROGRAMMING_H_INCLUDED
#include "../APP_UDS.h"

/** @brief Negotiate an uncompressed logical-region download using ALFID 0x44.
 * @note Runs in cyclic context. Input is owned by the server; the port must copy retained input.
 * @param[in,out] f_Context_ps Server. @param[in,out] f_Exchange_ps Request and bounded output.
 * @retval RC_OK Terminal. @retval RC_WARNING_PENDING Preparing staging region.
 */
t_eReturnCode APPUDS_ServiceDownload(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps);
/** @brief Commit a sequential block, acknowledging exact duplicates without a second write.
 * @note The server retains the active request while a port operation is pending.
 * @param[in,out] f_Context_ps Server. @param[in,out] f_Exchange_ps Borrowed request and output.
 * @retval RC_OK Terminal. @retval RC_WARNING_PENDING Durable write pending.
 */
t_eReturnCode APPUDS_ServiceTransfer(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps);
/** @brief Close a fully received image without making it bootable.
 * @param[in,out] f_Context_ps Server. @param[in,out] f_Exchange_ps Borrowed request and output.
 * @retval RC_OK Terminal. @retval RC_WARNING_PENDING Staging close pending.
 */
t_eReturnCode APPUDS_ServiceExit(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps);
/** @brief Verify an image manifest through a trusted port using a startRoutine request.
 * @param[in,out] f_Context_ps Server. @param[in,out] f_Exchange_ps Borrowed manifest and output.
 * @retval RC_OK Terminal. @retval RC_WARNING_PENDING Trusted verification pending.
 */
t_eReturnCode APPUDS_RoutineVerify(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps);
/** @brief Request activation only after trusted verification; simulated ports cannot activate hardware.
 * @param[in,out] f_Context_ps Server. @param[in,out] f_Exchange_ps Borrowed request and output.
 * @retval RC_OK Terminal. @retval RC_WARNING_PENDING Metadata commit pending.
 */
t_eReturnCode APPUDS_RoutineActivate(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps);

#endif
