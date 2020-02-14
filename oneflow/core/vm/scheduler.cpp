#include "oneflow/core/vm/scheduler.msg.h"
#include "oneflow/core/vm/control_vpu.h"
#include "oneflow/core/common/util.h"

namespace oneflow {

using WaitingVpuInstrCtxList = VpuScheduler::waiting_vpu_instr_ctx_list_ObjectMsgListType;
using ReadyVpuInstrCtxList = VpuScheduler::ready_vpu_instr_ctx_list_ObjectMsgListType;
using MaybeAvailableAccessList = VpuScheduler::maybe_available_access_list_ObjectMsgListType;
using TmpWaitingVpuInstrMsgList = VpuScheduler::tmp_waiting_msg_list_ObjectMsgListType;
using NewVpuInstrCtxList = VpuScheduler::new_vpu_instr_ctx_list_ObjectMsgListType;
using Id2LogicalObject = VpuScheduler::id2logical_object_ObjectMsgSkipListType;
using ActiveVpuCtxList = VpuScheduler::active_vpu_ctx_list_ObjectMsgListType;

namespace {

void ReleaseVpuInstructionCtx(VpuInstructionCtx* vpu_instr_ctx,
                              /*out*/ MaybeAvailableAccessList* maybe_available_access_list) {
  auto* holding_operand_list = vpu_instr_ctx->mut_holding_operand_list();
  OBJECT_MSG_LIST_FOR_EACH_PTR(holding_operand_list, mirrored_object_access) {
    auto* mirrored_object = mirrored_object_access->mut_mirrored_object();
    auto* holding_access_list = mirrored_object->mut_holding_access_list();
    holding_access_list->Erase(mirrored_object_access);
    if (!holding_access_list->empty()) { continue; }
    mirrored_object->clear_current_access_type();
    if (mirrored_object->waiting_access_list().empty()) {
      mirrored_object->logical_object().free_mirrored_object_handler().Call(
          mirrored_object->mut_logical_object());
      continue;
    }
    maybe_available_access_list->PushBack(mirrored_object);
  }
}

void ReleaseVpuInstructionPackage(RunningVpuInstructionPackage* pkg,
                                  /*out*/ MaybeAvailableAccessList* maybe_available_access_list) {
  auto* vpu_instr_ctx_list = pkg->mut_vpu_instruction_ctx_list();
  OBJECT_MSG_LIST_FOR_EACH_PTR(vpu_instr_ctx_list, vpu_instr_ctx) {
    ReleaseVpuInstructionCtx(vpu_instr_ctx, /*out*/ maybe_available_access_list);
    vpu_instr_ctx_list->Erase(vpu_instr_ctx);
  }
}

void TryReleaseFinishedVpuInstructionPackages(
    VpuSetCtx* vpu_set_ctx,
    /*out*/ MaybeAvailableAccessList* maybe_available_access_list) {
  auto* pkg_list = vpu_set_ctx->mut_launched_pkg_list();
  while (true) {
    auto* begin = pkg_list->Begin();
    if (begin == nullptr || !begin->Done()) { break; }
    ReleaseVpuInstructionPackage(begin, /*out*/ maybe_available_access_list);
    pkg_list->Erase(begin);
  }
}

void FilterReadyVpuInstrCtx(MaybeAvailableAccessList* maybe_available_access_list,
                            WaitingVpuInstrCtxList* waiting_vpu_instr_ctx_list,
                            /*out*/ ReadyVpuInstrCtxList* ready_vpu_instr_ctx_list) {
  OBJECT_MSG_LIST_FOR_EACH_PTR(maybe_available_access_list, mirrored_object) {
    mirrored_object->TryResetCurrentAccessType();
    auto* waiting_access_list = mirrored_object->mut_waiting_access_list();
    auto* holding_access_list = mirrored_object->mut_holding_access_list();
    while (auto* mirrored_object_access = mirrored_object->GetFirstAllowedAccess()) {
      waiting_access_list->MoveToDstBack(mirrored_object_access, holding_access_list);
      auto* vpu_instruction_ctx = mirrored_object_access->mut_vpu_instruction_ctx();
      auto* waiting_operand_list = vpu_instruction_ctx->mut_waiting_operand_list();
      auto* holding_operand_list = vpu_instruction_ctx->mut_holding_operand_list();
      waiting_operand_list->MoveToDstBack(mirrored_object_access, holding_operand_list);
      if (waiting_operand_list->empty()) {
        waiting_vpu_instr_ctx_list->MoveToDstBack(vpu_instruction_ctx, ready_vpu_instr_ctx_list);
      }
    }
    maybe_available_access_list->Erase(mirrored_object);
  }
}

void FilterAndRunControlVpuInstructions(VpuScheduler* scheduler,
                                        TmpWaitingVpuInstrMsgList* vpu_instr_msg_list) {
  ControlVpu control_vpu;
  OBJECT_MSG_LIST_FOR_EACH_PTR(vpu_instr_msg_list, vpu_instr_msg) {
    const VpuTypeId vpu_type_id = vpu_instr_msg->vpu_instruction_proto().vpu_type_id();
    if (vpu_type_id != kControlVpuTypeId) { continue; }
    control_vpu.Run(scheduler, vpu_instr_msg);
    vpu_instr_msg_list->Erase(vpu_instr_msg);
  }
}

void MakeVpuInstructionCtx(VpuScheduler* scheduler, TmpWaitingVpuInstrMsgList* vpu_instr_msg_list,
                           /*out*/ NewVpuInstrCtxList* ret_vpu_instr_ctx_list) {
  OBJECT_MSG_LIST_FOR_EACH_PTR(vpu_instr_msg_list, vpu_instr_msg) {
    VpuTypeId vpu_type_id = vpu_instr_msg->vpu_instruction_proto().vpu_type_id();
    auto* vpu_type_ctx = scheduler->mut_vpu_type_id2vpu_type_ctx()->FindPtr(vpu_type_id);
    OBJECT_MSG_LIST_FOR_EACH_UNSAFE_PTR(vpu_type_ctx->mut_vpu_ctx_list(), vpu_ctx) {
      auto vpu_instr_ctx = ObjectMsgPtr<VpuInstructionCtx>::NewFrom(
          scheduler->mut_default_allocator(), vpu_instr_msg, vpu_ctx);
      ret_vpu_instr_ctx_list->PushBack(vpu_instr_ctx.Mutable());
    }
    vpu_instr_msg_list->Erase(vpu_instr_msg);
  }
}

MirroredObject* FindMirroredObject(Id2LogicalObject* id2logical_object,
                                   const LogicalObjectId& logical_object_id, int64_t parallel_id) {
  auto* logical_object = id2logical_object->FindPtr(logical_object_id);
  CHECK_NOTNULL(logical_object);
  auto* ret = logical_object->mut_parallel_id2mirrored_object()->FindPtr(parallel_id);
  CHECK_NOTNULL(ret);
  return ret;
}

static const bool kConstOperandAccess = true;
static const bool kMutableOperandAccess = false;
template<bool is_const_operand>
void ConsumeMirroredObject(MirroredObject* mirrored_object, VpuInstructionCtx* vpu_instr_ctx) {
  uint64_t id_value = mirrored_object->logical_object().logical_object_id().value();
  auto mirrored_object_access = ObjectMsgPtr<MirroredObjectAccess>::NewFrom(
      vpu_instr_ctx->mut_allocator(), vpu_instr_ctx, mirrored_object, id_value, is_const_operand);
  mirrored_object->mut_waiting_access_list()->PushBack(mirrored_object_access.Mutable());
  vpu_instr_ctx->mut_waiting_operand_list()->PushBack(mirrored_object_access.Mutable());
  vpu_instr_ctx->mut_logical_object_id2operand_access()->Insert(mirrored_object_access.Mutable());
}

inline void TryPushBack(MaybeAvailableAccessList* maybe_available_access_list,
                        MirroredObject* mirrored_object) {
  if (mirrored_object->is_maybe_available_access_link_empty()) {
    maybe_available_access_list->PushBack(mirrored_object);
  }
}

void ConsumeMirroredObjects(Id2LogicalObject* id2logical_object,
                            NewVpuInstrCtxList* new_vpu_instr_ctx_list,
                            /*out*/ MaybeAvailableAccessList* maybe_available_access_list) {
  OBJECT_MSG_LIST_FOR_EACH_UNSAFE_PTR(new_vpu_instr_ctx_list, vpu_instr_ctx) {
    int64_t parallel_id = vpu_instr_ctx->vpu_ctx().vpu_id().parallel_id();
    const auto& operands = vpu_instr_ctx->vpu_instruction_msg().vpu_instruction_proto().operand();
    for (const auto& operand : operands) {
      if (operand.has_const_operand()) {
        auto* mirrored_object =
            FindMirroredObject(id2logical_object, operand.const_operand().value(), parallel_id);
        ConsumeMirroredObject<kConstOperandAccess>(mirrored_object, vpu_instr_ctx);
        TryPushBack(maybe_available_access_list, mirrored_object);
      } else if (operand.has_mutable_operand()) {
        auto* mirrored_object =
            FindMirroredObject(id2logical_object, operand.mutable_operand().value(), parallel_id);
        ConsumeMirroredObject<kMutableOperandAccess>(mirrored_object, vpu_instr_ctx);
        TryPushBack(maybe_available_access_list, mirrored_object);
      } else {
        // do nothing
      }
    }
  }
}

void MoveToReadyCtxListIfNoObjectOperand(NewVpuInstrCtxList* new_vpu_instr_ctx_list,
                                         /*out*/ ReadyVpuInstrCtxList* ready_vpu_instr_ctx_list) {
  OBJECT_MSG_LIST_FOR_EACH_PTR(new_vpu_instr_ctx_list, vpu_instr_ctx) {
    if (vpu_instr_ctx->waiting_operand_list().empty()) {
      new_vpu_instr_ctx_list->MoveToDstBack(vpu_instr_ctx, ready_vpu_instr_ctx_list);
    }
  }
}

void DispatchVpuInstructionCtx(VpuScheduler* scheduler,
                               ReadyVpuInstrCtxList* ready_vpu_instr_ctx_list) {
  auto* allocator = scheduler->mut_default_allocator();
  auto* active_vpu_ctx_list = scheduler->mut_active_vpu_ctx_list();
  while (auto* first = ready_vpu_instr_ctx_list->Begin()) {
    auto* vpu_ctx = first->mut_vpu_ctx();
    ready_vpu_instr_ctx_list->MoveToDstBack(first, vpu_ctx->mut_collect_vpu_instruction_list());
    if (vpu_ctx->is_active_vpu_ctx_link_empty()) { active_vpu_ctx_list->PushBack(vpu_ctx); }
  }
  OBJECT_MSG_LIST_FOR_EACH_PTR(active_vpu_ctx_list, vpu_ctx) {
    auto pkg = ObjectMsgPtr<RunningVpuInstructionPackage>::NewFrom(allocator, vpu_ctx);
    vpu_ctx->mut_collect_vpu_instruction_list()->MoveTo(pkg->mut_vpu_instruction_ctx_list());
    vpu_ctx->mut_vpu_set_ctx()->mut_launched_pkg_list()->PushBack(pkg.Mutable());
    vpu_ctx->mut_waiting_pkg_list()->EmplaceBack(std::move(pkg));
    active_vpu_ctx_list->Erase(vpu_ctx);
  }
}

}  // namespace

void VpuScheduler::Receive(VpuInstructionMsgList* vpu_instr_list) {
  mut_waiting_msg_list()->MoveFrom(vpu_instr_list);
}

void VpuScheduler::Dispatch() {
  auto* vpu_set_ctx_list = mut_vpu_set_ctx_list();
  auto* maybe_available_access_list = mut_maybe_available_access_list();
  OBJECT_MSG_LIST_FOR_EACH_UNSAFE_PTR(vpu_set_ctx_list, vpu_set_ctx) {
    TryReleaseFinishedVpuInstructionPackages(vpu_set_ctx, /*out*/ maybe_available_access_list);
  };
  auto* waiting_vpu_instr_ctx_list = mut_waiting_vpu_instr_ctx_list();
  auto* ready_vpu_instr_ctx_list = mut_ready_vpu_instr_ctx_list();
  if (waiting_msg_list().size() > 0) {
    auto* tmp_waiting_msg_list = mut_tmp_waiting_msg_list();
    mut_waiting_msg_list()->MoveTo(tmp_waiting_msg_list);
    FilterAndRunControlVpuInstructions(this, tmp_waiting_msg_list);
    auto* new_vpu_instr_ctx_list = mut_new_vpu_instr_ctx_list();
    MakeVpuInstructionCtx(this, tmp_waiting_msg_list, /*out*/ new_vpu_instr_ctx_list);
    ConsumeMirroredObjects(mut_id2logical_object(), new_vpu_instr_ctx_list,
                           /*out*/ maybe_available_access_list);
    MoveToReadyCtxListIfNoObjectOperand(new_vpu_instr_ctx_list, /*out*/ ready_vpu_instr_ctx_list);
    new_vpu_instr_ctx_list->MoveTo(waiting_vpu_instr_ctx_list);
  }
  FilterReadyVpuInstrCtx(maybe_available_access_list, waiting_vpu_instr_ctx_list,
                         /*out*/ ready_vpu_instr_ctx_list);
  DispatchVpuInstructionCtx(this, ready_vpu_instr_ctx_list);
}

}  // namespace oneflow