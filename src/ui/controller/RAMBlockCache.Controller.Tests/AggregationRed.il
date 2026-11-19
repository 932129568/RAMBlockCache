.assembly extern System.Private.CoreLib { .ver 8:0:0:0 }
.assembly extern System.Console { .ver 8:0:0:0 }
.assembly extern System.ObjectModel { .ver 8:0:0:0 }
.assembly extern RAMBlockCache.Controller { }
.assembly RAMBlockCache.Controller.Tests { }

.class private auto ansi beforefieldinit AggregationRed.Program
       extends [System.Private.CoreLib]System.Object
{
  .method private hidebysig static void SetField(object target, string name, object fieldValue) cil managed
  {
    .maxstack 5
    .locals init (class [System.Private.CoreLib]System.Reflection.FieldInfo fieldInfo)
    ldarg.0
    callvirt instance class [System.Private.CoreLib]System.Type [System.Private.CoreLib]System.Object::GetType()
    ldstr "<"
    ldarg.1
    ldstr ">k__BackingField"
    call string [System.Private.CoreLib]System.String::Concat(string, string, string)
    ldc.i4.s 36
    callvirt instance class [System.Private.CoreLib]System.Reflection.FieldInfo [System.Private.CoreLib]System.Type::GetField(string, valuetype [System.Private.CoreLib]System.Reflection.BindingFlags)
    stloc.0
    ldloc.0
    brtrue.s FIELD_OK
    ldarg.0
    callvirt instance class [System.Private.CoreLib]System.Type [System.Private.CoreLib]System.Object::GetType()
    ldarg.1
    ldc.i4.s 20
    callvirt instance class [System.Private.CoreLib]System.Reflection.FieldInfo [System.Private.CoreLib]System.Type::GetField(string, valuetype [System.Private.CoreLib]System.Reflection.BindingFlags)
    stloc.0
    ldloc.0
    brtrue.s FIELD_OK
    ldarg.1
    ldstr "missing backing field"
    newobj instance void [System.Private.CoreLib]System.MissingFieldException::.ctor(string, string)
    throw
  FIELD_OK:
    ldloc.0
    ldarg.0
    ldarg.2
    callvirt instance void [System.Private.CoreLib]System.Reflection.FieldInfo::SetValue(object, object)
    ret
  }

  .method private hidebysig static class [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.DriveItemViewModel
          CreateDrive(uint64 deviceKey, valuetype [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatusOutput shared) cil managed
  {
    .maxstack 6
    .locals init (
      class [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.DriveItemViewModel drive,
      class [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatsSnapshot sample,
      class [System.Private.CoreLib]System.Reflection.MethodInfo onSample)
    ldtoken [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.DriveItemViewModel
    call class [System.Private.CoreLib]System.Type [System.Private.CoreLib]System.Type::GetTypeFromHandle(valuetype [System.Private.CoreLib]System.RuntimeTypeHandle)
    call object [System.Private.CoreLib]System.Runtime.CompilerServices.RuntimeHelpers::GetUninitializedObject(class [System.Private.CoreLib]System.Type)
    castclass [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.DriveItemViewModel
    stloc.0
    ldloc.0
    ldstr "DeviceKey"
    ldarg.0
    box [System.Private.CoreLib]System.UInt64
    call void AggregationRed.Program::SetField(object, string, object)
    ldloc.0
    ldstr "DriveLetter"
    ldarg.0
    ldc.i4.1
    beq.s USE_D
    ldstr "E:"
    br.s SET_LETTER
  USE_D:
    ldstr "D:"
  SET_LETTER:
    call void AggregationRed.Program::SetField(object, string, object)

    newobj instance void [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatsSnapshot::.ctor()
    stloc.1
    ldloc.1
    ldstr "DeviceKey"
    ldarg.0
    box [System.Private.CoreLib]System.UInt64
    call void AggregationRed.Program::SetField(object, string, object)
    ldloc.1
    ldstr "CacheBytesUsed"
    ldarga.s shared
    ldfld uint64 [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatusOutput::CacheBytesUsed
    box [System.Private.CoreLib]System.UInt64
    call void AggregationRed.Program::SetField(object, string, object)
    ldloc.1
    ldstr "CacheBytesTotal"
    ldarga.s shared
    ldfld uint64 [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatusOutput::CacheBytesTotal
    box [System.Private.CoreLib]System.UInt64
    call void AggregationRed.Program::SetField(object, string, object)
    ldloc.1
    ldstr "BytesFromCache"
    ldarga.s shared
    ldfld uint64 [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatusOutput::BytesFromCache
    box [System.Private.CoreLib]System.UInt64
    call void AggregationRed.Program::SetField(object, string, object)
    ldloc.1
    ldstr "Mounted"
    ldarga.s shared
    ldfld uint32 [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatusOutput::Mounted
    ldc.i4.0
    cgt.un
    box [System.Private.CoreLib]System.Boolean
    call void AggregationRed.Program::SetField(object, string, object)
    ldloc.1
    ldstr "Mode"
    ldarga.s shared
    ldfld uint32 [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatusOutput::Mode
    box [System.Private.CoreLib]System.UInt32
    call void AggregationRed.Program::SetField(object, string, object)

    ldtoken [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.DriveItemViewModel
    call class [System.Private.CoreLib]System.Type [System.Private.CoreLib]System.Type::GetTypeFromHandle(valuetype [System.Private.CoreLib]System.RuntimeTypeHandle)
    ldstr "OnSample"
    ldc.i4.s 36
    callvirt instance class [System.Private.CoreLib]System.Reflection.MethodInfo [System.Private.CoreLib]System.Type::GetMethod(string, valuetype [System.Private.CoreLib]System.Reflection.BindingFlags)
    stloc.2
    ldloc.2
    ldloc.0
    ldc.i4.1
    newarr [System.Private.CoreLib]System.Object
    dup
    ldc.i4.0
    ldloc.1
    stelem.ref
    callvirt instance object [System.Private.CoreLib]System.Reflection.MethodBase::Invoke(object, object[])
    pop
    ldloc.0
    ret
  }

  .method public hidebysig static int32 Main() cil managed
  {
    .entrypoint
    .maxstack 7
    .locals init (
      valuetype [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatusOutput shared,
      class [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.DriveItemViewModel first,
      class [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.DriveItemViewModel second,
      class [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.MainViewModel vm,
      class [System.ObjectModel]System.Collections.ObjectModel.ObservableCollection`1<class [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.DriveItemViewModel> drives,
      class [System.Private.CoreLib]System.Reflection.MethodInfo refresh,
      uint64 expectedUsed,
      uint64 expectedServed,
      uint64 actualServed)
    ldloca.s shared
    call instance void [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatusOutput::.ctor()
    ldloca.s shared
    ldc.i8 268435456
    stfld uint64 [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatusOutput::CacheBytesUsed
    ldloca.s shared
    ldc.i8 1073741824
    stfld uint64 [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatusOutput::CacheBytesTotal
    ldloca.s shared
    ldc.i8 67108864
    stfld uint64 [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatusOutput::BytesFromCache
    ldloca.s shared
    ldc.i4.1
    stfld uint32 [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatusOutput::Mounted

    ldc.i4.1
    conv.u8
    ldloc.0
    call class [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.DriveItemViewModel AggregationRed.Program::CreateDrive(uint64, valuetype [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatusOutput)
    stloc.1
    ldc.i4.2
    conv.u8
    ldloc.0
    call class [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.DriveItemViewModel AggregationRed.Program::CreateDrive(uint64, valuetype [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatusOutput)
    stloc.2

    ldtoken [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.MainViewModel
    call class [System.Private.CoreLib]System.Type [System.Private.CoreLib]System.Type::GetTypeFromHandle(valuetype [System.Private.CoreLib]System.RuntimeTypeHandle)
    call object [System.Private.CoreLib]System.Runtime.CompilerServices.RuntimeHelpers::GetUninitializedObject(class [System.Private.CoreLib]System.Type)
    castclass [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.MainViewModel
    stloc.3
    newobj instance void class [System.ObjectModel]System.Collections.ObjectModel.ObservableCollection`1<class [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.DriveItemViewModel>::.ctor()
    stloc.s drives
    ldloc.s drives
    ldloc.1
    callvirt instance void class [System.Private.CoreLib]System.Collections.ObjectModel.Collection`1<class [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.DriveItemViewModel>::Add(!0)
    ldloc.s drives
    ldloc.2
    callvirt instance void class [System.Private.CoreLib]System.Collections.ObjectModel.Collection`1<class [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.DriveItemViewModel>::Add(!0)
    ldloc.3
    ldstr "Drives"
    ldloc.s drives
    call void AggregationRed.Program::SetField(object, string, object)

    ldtoken [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.MainViewModel
    call class [System.Private.CoreLib]System.Type [System.Private.CoreLib]System.Type::GetTypeFromHandle(valuetype [System.Private.CoreLib]System.RuntimeTypeHandle)
    ldstr "RefreshOverview"
    ldc.i4.s 36
    callvirt instance class [System.Private.CoreLib]System.Reflection.MethodInfo [System.Private.CoreLib]System.Type::GetMethod(string, valuetype [System.Private.CoreLib]System.Reflection.BindingFlags)
    stloc.s refresh
    ldloc.s refresh
    ldloc.3
    ldnull
    callvirt instance object [System.Private.CoreLib]System.Reflection.MethodBase::Invoke(object, object[])
    pop

    ldloc.0
    ldfld uint64 [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatusOutput::CacheBytesUsed
    stloc.s expectedUsed
    ldloc.0
    ldfld uint64 [RAMBlockCache.Controller]RAMBlockCache.Controller.Services.StatusOutput::BytesFromCache
    stloc.s expectedServed
    ldloc.3
    callvirt instance float64 [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.MainViewModel::get_ServedTotalGB()
    ldc.r8 1073741824.0
    mul
    conv.u8
    stloc.s actualServed

    ldstr "A5_PROOF mounted={0}"
    ldloc.3
    callvirt instance int32 [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.MainViewModel::get_MountedCount()
    box [System.Private.CoreLib]System.Int32
    call void [System.Console]System.Console::WriteLine(string, object)
    ldstr "A5_PROOF used={0}"
    ldloc.3
    callvirt instance uint64 [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.MainViewModel::get_CacheUsedTotal()
    box [System.Private.CoreLib]System.UInt64
    call void [System.Console]System.Console::WriteLine(string, object)
    ldstr "A5_PROOF served={0}"
    ldloc.s actualServed
    box [System.Private.CoreLib]System.UInt64
    call void [System.Console]System.Console::WriteLine(string, object)
    ldstr "A5_EXPECT used={0} served={1}"
    ldloc.s expectedUsed
    box [System.Private.CoreLib]System.UInt64
    ldloc.s expectedServed
    box [System.Private.CoreLib]System.UInt64
    call void [System.Console]System.Console::WriteLine(string, object, object)

    ldloc.3
    callvirt instance int32 [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.MainViewModel::get_MountedCount()
    ldc.i4.2
    bne.un SETUP_ERROR
    ldloc.3
    callvirt instance uint64 [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.MainViewModel::get_CacheUsedTotal()
    ldloc.s expectedUsed
    ldc.i4.2
    conv.u8
    mul
    bne.un SETUP_ERROR
    ldloc.s actualServed
    ldloc.s expectedServed
    ldc.i4.2
    conv.u8
    mul
    bne.un SETUP_ERROR

    ldloc.3
    callvirt instance uint64 [RAMBlockCache.Controller]RAMBlockCache.Controller.ViewModels.MainViewModel::get_CacheUsedTotal()
    ldloc.s expectedUsed
    bne.un RED
    ldloc.s actualServed
    ldloc.s expectedServed
    beq UNEXPECTED_GREEN
  RED:
    ldstr "RED A5: one engine-global StatusOutput was counted once per mounted volume"
    call void [System.Console]System.Console::WriteLine(string)
    ldc.i4.1
    ret
  UNEXPECTED_GREEN:
    ldstr "UNEXPECTED_GREEN A5: global values were counted once"
    call void [System.Console]System.Console::WriteLine(string)
    ldc.i4.0
    ret
  SETUP_ERROR:
    ldstr "SETUP_ERROR: fixture did not reproduce exact 2x aggregation"
    call void [System.Console]System.Console::WriteLine(string)
    ldc.i4.2
    ret
  }
}
