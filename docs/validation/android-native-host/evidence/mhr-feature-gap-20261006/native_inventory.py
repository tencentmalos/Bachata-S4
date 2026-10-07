# qrenderdoc --python inventory.py : full action/pipeline/shader inventory of the native MHR frame.
import renderdoc as rd, os, json, re, traceback

OUT = r"C:\Users\fangfang\AppData\Local\Temp\claude\D--workspace-shadps4\c5d6af24-eadc-40cd-ac29-4cf8956f47a1\scratchpad\rdc\inv"
CAP = r"D:\workspace\xrgame-native-evidence\api-replay\run-20261005\rdoc\mhr_save_select_frame2.rdc"
os.makedirs(os.path.join(OUT, "shaders"), exist_ok=True)
log = open(os.path.join(OUT, "log.txt"), "w")


def L(*a):
    log.write(" ".join(str(x) for x in a) + "\n")
    log.flush()


def rid(x):
    return str(x).replace("ResourceId::", "")


def name_of(action, sf):
    try:
        return action.GetName(sf)
    except Exception:
        return getattr(action, "customName", "") or getattr(action, "name", "")


def desc_resource(d):
    # New API: UsedDescriptor/Descriptor; old API: BoundResource(Array)
    for path in (("descriptor", "resource"), ("resource",), ("resourceId",)):
        o = d
        try:
            for p in path:
                o = getattr(o, p)
            return rid(o)
        except Exception:
            continue
    try:
        return rid(d.resources[0].resourceId)
    except Exception:
        return None


def desc_slot(d):
    for path in (("access", "index"), ("access", "arrayElement"), ("bindPoint",)):
        o = d
        try:
            for p in path:
                o = getattr(o, p)
            return int(o)
        except Exception:
            continue
    return -1


def run(ctrl):
    sf = ctrl.GetStructuredFile()
    targets = ctrl.GetDisassemblyTargets(True)
    L("targets", list(targets))
    textures = {}
    for t in ctrl.GetTextures():
        textures[rid(t.resourceId)] = {
            "fmt": str(t.format.Name()), "w": t.width, "h": t.height, "d": t.depth,
            "mips": t.mips, "arr": t.arraysize, "ms": t.msSamp, "type": str(t.type),
            "flags": str(t.creationFlags), "bytes": t.byteSize}
    buffers = {}
    for b in ctrl.GetBuffers():
        buffers[rid(b.resourceId)] = {"len": b.length, "flags": str(b.creationFlags)}
    names = {}
    for r in ctrl.GetResources():
        names[rid(r.resourceId)] = r.name
    json.dump({"textures": textures, "buffers": buffers, "names": names},
              open(os.path.join(OUT, "resources.json"), "w"), indent=0)
    L("resources", len(textures), len(buffers), len(names))

    actions = []

    def walk(lst, depth, parent):
        for a in lst:
            actions.append((a, depth, parent))
            if len(a.children):
                walk(a.children, depth + 1, a.eventId)

    walk(ctrl.GetRootActions(), 0, 0)
    L("actions", len(actions))

    shaders = {}
    events = []
    first = True
    for a, depth, parent in actions:
        flags = a.flags
        row = {"eid": a.eventId, "depth": depth, "parent": parent, "name": name_of(a, sf),
               "flags": str(flags), "nidx": a.numIndices, "ninst": a.numInstances,
               "disp": list(a.dispatchDimension), "outputs": [rid(o) for o in a.outputs],
               "depth_out": rid(a.depthOut), "copy_src": rid(a.copySource),
               "copy_dst": rid(a.copyDestination)}
        is_work = bool(flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch))
        if is_work:
            try:
                ctrl.SetFrameEvent(a.eventId, False)
                st = ctrl.GetPipelineState()
                d12 = ctrl.GetD3D12PipelineState()
                if first:
                    L("pipestate dir", [x for x in dir(st) if not x.startswith("_")])
                    L("d12 dir", [x for x in dir(d12) if not x.startswith("_")])
                stages = {}
                for stage, nm in ((rd.ShaderStage.Vertex, "vs"), (rd.ShaderStage.Hull, "hs"),
                                  (rd.ShaderStage.Domain, "ds"), (rd.ShaderStage.Geometry, "gs"),
                                  (rd.ShaderStage.Pixel, "ps"), (rd.ShaderStage.Compute, "cs")):
                    refl = st.GetShaderReflection(stage)
                    if refl is None:
                        continue
                    sid = rid(refl.resourceId)
                    stages[nm] = sid
                    # bound resources by slot
                    try:
                        ro = st.GetReadOnlyResources(stage)
                        rw = st.GetReadWriteResources(stage)
                        if first:
                            L("ro elem dir", [x for x in dir(ro[0]) if not x.startswith("_")] if len(ro) else None)
                        row[nm + "_ro"] = [(desc_slot(x), desc_resource(x)) for x in ro]
                        row[nm + "_rw"] = [(desc_slot(x), desc_resource(x)) for x in rw]
                    except Exception as e:
                        row[nm + "_bind_err"] = str(e)
                    if sid not in shaders:
                        info = {"stage": nm, "first_event": a.eventId}
                        try:
                            info["inputs"] = [(s.semanticName, s.semanticIndex, str(s.varType), s.compCount) for s in refl.inputSignature]
                            info["outputs"] = [(s.semanticName, s.semanticIndex, str(s.varType), s.compCount) for s in refl.outputSignature]
                            def res_list(lst):
                                out = []
                                for r in lst:
                                    t = getattr(r, "textureType", None) or getattr(r, "resType", None)
                                    out.append((r.name, str(t), bool(getattr(r, "isTexture", False))))
                                return out
                            info["ro"] = res_list(refl.readOnlyResources)
                            info["rw"] = res_list(refl.readWriteResources)
                            info["cb"] = [(c.name, c.byteSize) for c in refl.constantBlocks]
                            info["samplers"] = [s.name for s in refl.samplers]
                            info["threads"] = list(refl.dispatchThreadsDimension)
                            info["debug_name"] = refl.debugInfo.files[0].filename if len(refl.debugInfo.files) else ""
                        except Exception as e:
                            info["refl_err"] = str(e)
                        try:
                            pipe = st.GetComputePipelineObject() if nm == "cs" else st.GetGraphicsPipelineObject()
                            dis = ctrl.DisassembleShader(pipe, refl, targets[0])
                        except Exception as e:
                            dis = "ERR %s" % e
                        with open(os.path.join(OUT, "shaders", "%s_%s.txt" % (nm, sid)), "w") as f:
                            f.write(dis)
                        info["dxop"] = {}
                        for m in re.finditer(r"@dx\.op\.([A-Za-z0-9]+)", dis):
                            info["dxop"][m.group(1)] = info["dxop"].get(m.group(1), 0) + 1
                        req = re.search(r"shader requires additional functionality:\n((?:;\s+.*\n)+)", dis)
                        info["requires"] = [x.strip("; ").strip() for x in req.group(1).splitlines()] if req else []
                        sm = re.search(r'!dx\.shaderModel = !\{!"(\w+)", i32 (\d+), i32 (\d+)\}', dis)
                        info["sm"] = "%s_%s_%s" % sm.groups() if sm else ""
                        info["groupshared"] = len(re.findall(r"addrspace\(3\)", dis))
                        info["half"] = len(re.findall(r"\bhalf\b", dis))
                        info["i16"] = len(re.findall(r"\bi16\b", dis))
                        info["i64"] = len(re.findall(r"\bi64\b", dis))
                        info["double"] = len(re.findall(r"\bdouble\b", dis))
                        info["lines"] = dis.count("\n")
                        shaders[sid] = info
                row["stages"] = stages
                try:
                    om = d12.outputMerger
                    dss = om.depthStencilState
                    row["depth"] = {"en": dss.depthEnable, "func": str(dss.depthFunction),
                                    "write": dss.depthWrites, "stencil": dss.stencilEnable}
                    bl = om.blendState.blends
                    row["blend"] = [(b.enabled, str(b.colorBlend.source), str(b.colorBlend.destination),
                                     str(b.colorBlend.operation), int(b.writeMask)) for b in bl[:8]]
                    row["rts"] = [desc_resource(x) for x in om.renderTargets]
                    row["ds"] = desc_resource(om.depthTarget)
                except Exception as e:
                    row["om_err"] = str(e)
                try:
                    rs = d12.rasterizer.state
                    row["rast"] = {"cull": str(rs.cullMode), "fill": str(rs.fillMode),
                                   "cons": str(rs.conservativeRasterization),
                                   "bias": rs.depthBias, "clip": rs.depthClip,
                                   "fsc": getattr(rs, "forcedSampleCount", None)}
                    vp = d12.rasterizer.viewports
                    if len(vp):
                        row["vp"] = [vp[0].x, vp[0].y, vp[0].width, vp[0].height, vp[0].minDepth, vp[0].maxDepth]
                except Exception as e:
                    row["rs_err"] = str(e)
                try:
                    row["topo"] = str(st.GetPrimitiveTopology())
                except Exception:
                    pass
                first = False
            except Exception:
                row["err"] = traceback.format_exc()[-400:]
        events.append(row)
    json.dump(events, open(os.path.join(OUT, "events.json"), "w"), indent=0)
    json.dump(shaders, open(os.path.join(OUT, "shaders.json"), "w"), indent=1)
    L("events", len(events), "shaders", len(shaders))


try:
    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
except Exception as e:
    L("init", e)
try:
    cap = rd.OpenCaptureFile()
    r = cap.OpenFile(CAP, "", None)
    L("open", r)
    res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    L("opencap", res)
    run(ctrl)
    ctrl.Shutdown()
    cap.Shutdown()
except Exception:
    L(traceback.format_exc())
L("done")
log.close()
os._exit(0)
