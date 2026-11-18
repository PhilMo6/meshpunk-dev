/* Linker script fragment for the Xtensa build of meshcore.loraproto.elf
   (build.ps1 passes it with -Wl,-T). It collects the listed input sections
   into .iram.text, the section lib/elf_loader copies into internal SRAM at
   load on Xtensa. -ffunction-sections names one input section per function;
   -mtext-section-literals puts each function's literal pool in that same
   section. A -T script that ends in INSERT augments ld's default script, and
   its input-section assignments are made before the default script's.

   Listed: mc_loop (loraproto_ops.loop) and the functions it calls on a pass
   that neither receives nor sends a packet, plus
   PunkMesh::checkPendingRepeats, which PunkMesh::loop calls while
   msg_repeat_enabled is set.

   build.ps1 fails the build when a listed function is not in the section. */
SECTIONS
{
  .iram.text :
  {
    /* package entry + PunkMesh */
    *(.text._ZL7mc_loopv)
    *(.text._ZN8PunkMesh4loopEv)
    *(.text._ZN8PunkMesh19checkPendingRepeatsEv)
    /* lib/MeshCore: BaseChatMesh, Mesh, Dispatcher */
    *(.text._ZN12BaseChatMesh4loopEv)
    *(.text._ZN12BaseChatMesh16checkConnectionsEv)
    *(.text._ZN12BaseChatMesh15hasConnectionToEPKh)
    *(.text._ZN4mesh4Mesh4loopEv)
    *(.text._ZN4mesh10Dispatcher4loopEv)
    *(.text._ZN4mesh10Dispatcher9checkRecvEv)
    *(.text._ZN4mesh10Dispatcher9checkSendEv)
    *(.text._ZNK4mesh10Dispatcher18millisHasNowPassedEm)
    *(.text._ZNK4mesh10Dispatcher12futureMillisEi)
    *(.text._ZNK4mesh10Dispatcher19getAGCResetIntervalEv)
    *(.text._ZNK4mesh10Dispatcher24getInterferenceThresholdEv)
    /* packet queues */
    *(.text._ZN23StaticPoolPacketManager14getNextInboundEj)
    *(.text._ZNK23StaticPoolPacketManager16getOutboundCountEj)
    *(.text._ZN11PacketQueue3getEj)
    *(.text._ZNK11PacketQueue11countBeforeEj)
    /* clock + serial shims */
    *(.text._ZN13ArduinoMillis9getMillisEv)
    *(.text._Z6millisv)
    *(.text._ZN12MpSerialStub9availableEv)
    *(.text._ZN5McRTC14getCurrentTimeEv)
    /* polled radio */
    *(.text._ZN13McPolledRadio4loopEv)
    *(.text._ZN13McPolledRadio7recvRawEPhi)
    *(.text._ZNK13McPolledRadio12isInRecvModeEv)
    *(.text._ZN13McPolledRadio26triggerNoiseFloorCalibrateEi)
    *(.text._ZN13McPolledRadio17isReceivingPacketEv*)
  }
}
INSERT AFTER .text;
