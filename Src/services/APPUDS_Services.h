/** @file APPUDS_Services.h @brief Compiled standard service handlers and safe default hooks. */
#ifndef APPUDS_SERVICES_H_INCLUDED
#define APPUDS_SERVICES_H_INCLUDED
#include "../APP_UDS.h"

/** @brief Handle DiagnosticSessionControl; borrowed exchange buffers are never retained.
 * @param[in,out] f_Context_ps Server. @param[in,out] f_Exchange_ps Request and bounded output.
 * @retval RC_OK Terminal positive or NRC response.
 */
t_eReturnCode APPUDS_ServiceSession(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps);
/** @brief Handle TesterPresent in cyclic context without retaining buffers.
 * @param[in,out] f_Context_ps Server. @param[in,out] f_Exchange_ps Request and output.
 * @retval RC_OK Terminal positive or NRC response.
 */
t_eReturnCode APPUDS_ServiceTester(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps);
/** @brief Read one or more DIDs, preflighting all lengths and rights before snapshots.
 * @param[in,out] f_Context_ps Server. @param[in,out] f_Exchange_ps Borrowed request and output.
 * @retval RC_OK Terminal positive or NRC response.
 */
t_eReturnCode APPUDS_ServiceRead(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps);
/** @brief Validate and atomically apply one DID; buffers are never retained.
 * @param[in,out] f_Context_ps Server. @param[in,out] f_Exchange_ps Borrowed request and output.
 * @retval RC_OK Terminal positive or NRC response.
 */
t_eReturnCode APPUDS_ServiceWrite(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps);
/** @brief Delegate authorization to a trusted application backend; null backend denies.
 * @param[in,out] f_Context_ps Server. @param[in,out] f_Exchange_ps Borrowed request and output.
 * @retval RC_OK Terminal. @retval RC_WARNING_PENDING Backend pending.
 */
t_eReturnCode APPUDS_ServiceSecurity(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps);
/** @brief Deny reset until a board port can prove safe state and physical response completion.
 * @param[in,out] f_Context_ps Server. @param[in,out] f_Exchange_ps Borrowed request and output.
 * @retval RC_OK Terminal NRC.
 */
t_eReturnCode APPUDS_ServiceReset(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps);
/** @brief Dispatch a registered bounded routine in cyclic context; no buffer is retained by the dispatcher.
 * @param[in,out] f_Context_ps Server. @param[in,out] f_Exchange_ps Borrowed request and output.
 * @retval RC_OK Terminal. @retval RC_WARNING_PENDING Routine pending.
 */
t_eReturnCode APPUDS_ServiceRoutine(t_sAPPUDS_Context * f_Context_ps, t_sAPPUDS_Exchange * f_Exchange_ps);
/** @brief Deny maintenance by default.
 * @param[in] f_Key_u16 Business identifier. @retval RC_ERROR_NOT_ALLOWED No authorization policy installed.
 */
t_eReturnCode APPUDS_DenyMaintenance(t_uint16 f_Key_u16);

#endif
