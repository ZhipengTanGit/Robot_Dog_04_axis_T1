using System;
using System.IO;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Web.Script.Serialization;
using SolidWorks.Interop.sldworks;

// All SOLIDWORKS calls below are getters. No Save/SaveAs, activation, rebuild,
// visibility, display, material, transform, selection or configuration changes.
public static class CadReadOnlyExport {
    static Dictionary<string,object> Obj(params object[] values) {
        Dictionary<string,object> d = new Dictionary<string,object>();
        for (int i=0;i<values.Length;i+=2) d.Add((string)values[i],values[i+1]);
        return d;
    }
    static float[] Floats(object raw) {
        if(raw==null) return new float[0];
        float[] typed=raw as float[]; if(typed!=null) return typed;
        Array array=(Array)raw; float[] result=new float[array.Length]; int i=0;
        foreach(object value in array) result[i++]=Convert.ToSingle(value);
        return result;
    }
    static double[] Identity() { return new double[]{1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0}; }
    static bool Visible(IComponent2 c) {
        for(IComponent2 node=c;node!=null;node=(IComponent2)node.GetParent())
            if(node.IsSuppressed()||node.IsHidden(false)||node.Visible==0) return false;
        return true;
    }
    static string Group(string part) {
        if(part.StartsWith("打印件_腿部_")) return "legs";
        if(part.StartsWith("打印件_")&&part.Contains("按钮")) return "accent";
        if(part.StartsWith("打印件_")) return "shell";
        return "fixed";
    }
    static string Color(string group,string part) {
        if(group=="shell")return "#d4ded8";
        if(group=="legs")return "#525c57";
        if(group=="accent")return "#f18a54";
        if(part.StartsWith("Board~"))return "#245844";
        return "#202429";
    }
    // Official appearance precedence: component > face > feature > body > part.
    // Unassigned getters can return null OR nine -1 values; both inherit.
    static string MaterialColor(object raw) {
        Array values=raw as Array;
        if(values==null||values.Length<3)return null;
        int[] rgb=new int[3];
        for(int i=0;i<3;i++){
            double value=Convert.ToDouble(values.GetValue(i));
            if(Double.IsNaN(value)||value<0||value>1)return null;
            rgb[i]=(int)Math.Round(value*255,MidpointRounding.AwayFromZero);
        }
        return "#"+rgb[0].ToString("x2")+rgb[1].ToString("x2")+rgb[2].ToString("x2");
    }
    static string FaceColor(IFace2 face,string componentColor,string bodyColor,string partColor,
        string fallback,Dictionary<string,string> featureColors,out string source) {
        if(componentColor!=null){source="component";return componentColor;}
        string color=MaterialColor(face.GetMaterialPropertyValues2(1,null));
        if(color!=null){source="face";return color;}
        IFeature feature=(IFeature)face.GetFeature();
        if(feature!=null){
            string featureName=feature.Name;
            if(!featureColors.TryGetValue(featureName,out color)){
                color=MaterialColor(feature.GetMaterialPropertyValues2(1,null));
                featureColors[featureName]=color;
            }
            if(color!=null){source="feature";return color;}
        }
        if(bodyColor!=null){source="body";return bodyColor;}
        if(partColor!=null){source="part";return partColor;}
        source="fallback";return fallback;
    }
    static string FaceGroup(string group,string part,string color) {
        if(group=="fixed")return "fixed";
        // Probe verified this exact assigned CAD color on the frame corners
        // and leg inlays. Assignment comes from appearance, never geometry.
        bool cadRed=String.Equals(color,"#b11919",StringComparison.OrdinalIgnoreCase);
        if(cadRed&&part.StartsWith("打印件_屏幕总成_"))return "frameAccent";
        if(cadRed&&part.StartsWith("打印件_腿部_"))return "legAccent";
        return group;
    }
    static bool Included(string path,string name,bool includeElectronics) {
        if(!path.EndsWith(".SLDPRT",StringComparison.OrdinalIgnoreCase))return false;
        // Retain the board substrate; omit internal ICs/passives/connectors.
        if(!includeElectronics&&name.Contains("装配体_PCB主板")&&!Path.GetFileName(path).StartsWith("Board~"))return false;
        return true;
    }
    static void Bounds(float[] xyz,double[] low,double[] high) {
        for(int i=0;i<xyz.Length;i++) {
            int axis=i%3; double value=xyz[i];
            if(Double.IsNaN(value)||Double.IsInfinity(value))throw new Exception("Non-finite CAD vertex.");
            low[axis]=Math.Min(low[axis],value);high[axis]=Math.Max(high[axis],value);
        }
    }
    static float[] Transform(float[] values,double[] m,bool normals) {
        float[] output=new float[values.Length];
        for(int i=0;i<values.Length;i+=3){
            double x=values[i],y=values[i+1],z=values[i+2];
            double xx=m[0]*x+m[3]*y+m[6]*z;
            double yy=m[1]*x+m[4]*y+m[7]*z;
            double zz=m[2]*x+m[5]*y+m[8]*z;
            if(normals){
                double length=Math.Sqrt(xx*xx+yy*yy+zz*zz);
                double factor=length>1e-12?(m[12]<0?-1:1)/length:0;
                xx*=factor;yy*=factor;zz*=factor;
            }else{xx=xx*m[12]+m[9];yy=yy*m[12]+m[10];zz=zz*m[12]+m[11];}
            output[i]=(float)xx;output[i+1]=(float)yy;output[i+2]=(float)zz;
        }
        return output;
    }
    static float[] FaceNormals(float[] p) {
        float[] result=new float[p.Length];
        for(int i=0;i<p.Length;i+=9){
            double ax=p[i+3]-p[i],ay=p[i+4]-p[i+1],az=p[i+5]-p[i+2];
            double bx=p[i+6]-p[i],by=p[i+7]-p[i+1],bz=p[i+8]-p[i+2];
            double x=ay*bz-az*by,y=az*bx-ax*bz,z=ax*by-ay*bx;
            double length=Math.Sqrt(x*x+y*y+z*z); if(length>1e-15){x/=length;y/=length;z/=length;}
            for(int j=0;j<3;j++){result[i+j*3]=(float)x;result[i+j*3+1]=(float)y;result[i+j*3+2]=(float)z;}
        }
        return result;
    }
    static void AlignWinding(float[] p,float[] n) {
        for(int i=0;i<p.Length;i+=9){
            double ax=p[i+3]-p[i],ay=p[i+4]-p[i+1],az=p[i+5]-p[i+2];
            double bx=p[i+6]-p[i],by=p[i+7]-p[i+1],bz=p[i+8]-p[i+2];
            double dot=(ay*bz-az*by)*(n[i]+n[i+3]+n[i+6])+
                (az*bx-ax*bz)*(n[i+1]+n[i+4]+n[i+7])+(ax*by-ay*bx)*(n[i+2]+n[i+5]+n[i+8]);
            if(dot<0)for(int j=0;j<3;j++){
                float t=p[i+3+j];p[i+3+j]=p[i+6+j];p[i+6+j]=t;
                t=n[i+3+j];n[i+3+j]=n[i+6+j];n[i+6+j]=t;
            }
        }
    }
    static void WriteFloatArray(BinaryWriter writer,float[] values) {
        byte[] bytes=new byte[values.Length*4];Buffer.BlockCopy(values,0,bytes,0,bytes.Length);writer.Write(bytes);
    }
    public static string Run(string assemblyPath,string outputDirectory,string diagnosticsDirectory) {
        return Run(assemblyPath,outputDirectory,diagnosticsDirectory,false);
    }
    public static string Run(string assemblyPath,string outputDirectory,string diagnosticsDirectory,bool includeElectronics) {
        ISldWorks app=(ISldWorks)Marshal.GetActiveObject("SldWorks.Application");
        IModelDoc2 active=(IModelDoc2)app.ActiveDoc;
        string activeBefore=active==null?"":active.GetPathName();
        IModelDoc2 doc=(IModelDoc2)app.GetOpenDocumentByName(assemblyPath);
        if(doc==null||!String.Equals(activeBefore,assemblyPath,StringComparison.OrdinalIgnoreCase))
            throw new Exception("Target must already be the active assembly; exporter does not activate documents.");
        bool dirtyBefore=doc.GetSaveFlag();
        IAssemblyDoc assembly=(IAssemblyDoc)doc;
        object[] components=(object[])assembly.GetComponents(false);
        List<object> meshes=new List<object>(),diagnostics=new List<object>(),skipped=new List<object>();
        double[] totalLow={Double.PositiveInfinity,Double.PositiveInfinity,Double.PositiveInfinity};
        double[] totalHigh={Double.NegativeInfinity,Double.NegativeInfinity,Double.NegativeInfinity};
        int triangles=0;
        string binaryPath=Path.Combine(outputDirectory,"model.bin");
        using(BinaryWriter writer=new BinaryWriter(File.Create(binaryPath))){
            foreach(object item in components){
                IComponent2 c=(IComponent2)item;
                string name=c.Name2,path=c.GetPathName();
                if(!Included(path,name,includeElectronics)||!Visible(c))continue;
                string part=Path.GetFileName(path),group=Group(part);
                Console.WriteLine("Reading mesh: "+name);
                IMathTransform transform=(IMathTransform)c.Transform2;
                double[] matrix=transform==null?Identity():(double[])transform.ArrayData;
                List<float> positions=new List<float>(),normals=new List<float>();
                List<object> faceRanges=new List<object>();
                Dictionary<string,string> featureColors=new Dictionary<string,string>();
                string componentColor=MaterialColor(c.GetMaterialPropertyValues2(1,null));
                string partColor=MaterialColor(c.GetModelMaterialPropertyValues(c.ReferencedConfiguration));
                IModelDoc2 model=(IModelDoc2)c.GetModelDoc2();
                if(partColor==null&&model!=null)partColor=MaterialColor(model.MaterialPropertyValues);
                string referencedConfiguration=c.ReferencedConfiguration;
                string referencedDisplayState=c.ReferencedDisplayState2;
                object bodyInfo;
                object[] bodies=c.GetBodies3(-1,out bodyInfo) as object[];
                int facesCount=0,emptyFaces=0,visibleBodies=0;
                if(bodies!=null)foreach(object bodyItem in bodies){
                    IBody2 body=(IBody2)bodyItem;
                    if(!body.Visible)continue;
                    ++visibleBodies;
                    string bodyColor=MaterialColor(body.MaterialPropertyValues2);
                    object[] faces=body.GetFaces() as object[];
                    if(faces==null)continue;
                    foreach(object faceItem in faces){
                        IFace2 face=(IFace2)faceItem;++facesCount;
                        float[] p=Floats(face.GetTessTriangles(true));
                        if(p.Length==0){++emptyFaces;continue;}
                        if(p.Length%9!=0)throw new Exception("Incomplete triangle data for "+name);
                        float[] n=Floats(face.GetTessNorms());
                        if(n.Length!=p.Length)n=FaceNormals(p);
                        p=Transform(p,matrix,false);n=Transform(n,matrix,true);AlignWinding(p,n);
                        string colorSource;
                        string faceColor=FaceColor(face,componentColor,bodyColor,partColor,Color(group,part),featureColors,out colorSource);
                        faceRanges.Add(Obj("id",name+"/f"+(facesCount-1),"firstVertex",positions.Count/3,
                            "count",p.Length/3,"color",faceColor,"group",FaceGroup(group,part,faceColor),"colorSource",colorSource));
                        positions.AddRange(p);normals.AddRange(n);
                    }
                }
                if(positions.Count==0){skipped.Add(Obj("name",name,"reason","no visible tessellated bodies","bodyCount",bodies==null?0:bodies.Length));continue;}
                float[] points=positions.ToArray(),normal=normals.ToArray();
                double[] low={Double.PositiveInfinity,Double.PositiveInfinity,Double.PositiveInfinity};
                double[] high={Double.NegativeInfinity,Double.NegativeInfinity,Double.NegativeInfinity};
                Bounds(points,low,high);Bounds(points,totalLow,totalHigh);
                long po=writer.BaseStream.Position;WriteFloatArray(writer,points);
                long no=writer.BaseStream.Position;WriteFloatArray(writer,normal);
                int count=points.Length/3;triangles+=count/3;
                meshes.Add(Obj("name",name,"part",part,"group",group,"color",componentColor??partColor??Color(group,part),
                    "positions",Obj("byteOffset",po,"count",count),"normals",Obj("byteOffset",no,"count",count),
                    "bounds",Obj("min",low,"max",high),"faces",faceRanges));
                diagnostics.Add(Obj("name",name,"partPath",path,"transform",matrix,"componentBox",c.GetBox(false,false),
                    "meshBounds",Obj("min",low,"max",high),"bodyCount",visibleBodies,"faces",facesCount,"emptyFaces",emptyFaces,"triangles",count/3,
                    "referencedConfiguration",referencedConfiguration,"referencedDisplayState",referencedDisplayState,
                    "componentColor",componentColor,"partColor",partColor));
                Console.WriteLine("  "+count/3+" triangles, "+facesCount+" faces");
            }
        }
        active=(IModelDoc2)app.ActiveDoc;
        string activeAfter=active==null?"":active.GetPathName();bool dirtyAfter=doc.GetSaveFlag();
        if(activeBefore!=activeAfter||dirtyBefore!=dirtyAfter)throw new Exception("CAD document state changed during read-only extraction; inspect diagnostics.");
        if(meshes.Count==0)throw new Exception("No geometry exported.");
        long bytes=new FileInfo(binaryPath).Length;
        JavaScriptSerializer json=new JavaScriptSerializer();json.MaxJsonLength=Int32.MaxValue;
        File.WriteAllText(Path.Combine(outputDirectory,"model.json"),json.Serialize(Obj(
            "version",1,"units","m","coordinateSystem","SOLIDWORKS assembly XYZ; component transforms baked once", 
            "source",Obj("assembly",Path.GetFileName(assemblyPath)),"bounds",Obj("min",totalLow,"max",totalHigh),
            "buffer","model.bin","byteLength",bytes,"triangleCount",triangles,"meshes",meshes,
            "faceColors","CAD component / face / feature / body / part appearance inheritance; explicit red accents retained",
            "omissions",includeElectronics?new string[0]:new string[]{"Internal PCB electronic components omitted; substrate retained."})),new System.Text.UTF8Encoding(false));
        File.WriteAllText(Path.Combine(diagnosticsDirectory,"export.json"),json.Serialize(Obj(
            "source",assemblyPath,"activeBefore",activeBefore,"activeAfter",activeAfter,"dirtyBefore",dirtyBefore,"dirtyAfter",dirtyAfter,
            "meshes",diagnostics,"skipped",skipped)),new System.Text.UTF8Encoding(false));
        return "Exported "+meshes.Count+" meshes, "+triangles+" triangles, "+bytes+" bytes; original CAD state unchanged.";
    }
}
