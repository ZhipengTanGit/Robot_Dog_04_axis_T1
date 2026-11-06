using System;
using System.IO;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Web.Script.Serialization;
using SolidWorks.Interop.sldworks;
public static class CadFeatureProbe {
    static Dictionary<string,object> D(params object[] values){Dictionary<string,object> d=new Dictionary<string,object>();for(int i=0;i<values.Length;i+=2)d[(string)values[i]]=values[i+1];return d;}
    static IComponent2 Find(IAssemblyDoc assembly,string name){
        string[] segments=name.Split('/');IComponent2 c=(IComponent2)assembly.GetComponentByName(segments[0]);
        for(int i=1;i<segments.Length&&c!=null;i++){
            IComponent2 found=null;foreach(object item in (object[])c.GetChildren()){
                IComponent2 child=(IComponent2)item;
                if(child.Name2.EndsWith("/"+segments[i])){found=child;break;}
            }c=found;
        }return c;
    }
    public static string Run(string path,string output){
        ISldWorks sw=(ISldWorks)Marshal.GetActiveObject("SldWorks.Application");
        IModelDoc2 doc=(IModelDoc2)sw.GetOpenDocumentByName(path);if(doc==null)throw new Exception("Target not already open.");
        bool dirtyBefore=doc.GetSaveFlag();IAssemblyDoc assembly=(IAssemblyDoc)doc;List<object> parts=new List<object>();
        foreach(string name in new string[]{"打印件_身体_上盖板-1","装配体_屏幕总成-1/打印件_屏幕总成_1-1","打印件_腿部_左腿-1","打印件_腿部_右腿-1"}){
            IComponent2 c=Find(assembly,name);if(c==null)throw new Exception("Component not found: "+name);
            Console.WriteLine("Read-only feature map: "+name);
            List<object> faces=new List<object>();object info;int id=0;Dictionary<string,string> types=new Dictionary<string,string>();
            foreach(object item in (object[])c.GetBodies3(-1,out info)){
                IBody2 body=(IBody2)item;if(!body.Visible)continue;
                foreach(object fr in (object[])body.GetFaces()){
                    IFace2 face=(IFace2)fr;IFeature feature=(IFeature)face.GetFeature();
                    string featureName=feature==null?null:feature.Name;
                    if(featureName!=null&&!types.ContainsKey(featureName))types[featureName]=feature.GetTypeName2();
                    ISurface surface=(ISurface)face.GetSurface();
                    faces.Add(D("index",id++,"feature",featureName,"box",face.GetBox(),
                        "cylinder",surface!=null&&surface.IsCylinder()?surface.CylinderParams:null));
                }
            }
            parts.Add(D("name",name,"features",types,"faces",faces));
        }
        JavaScriptSerializer json=new JavaScriptSerializer();json.MaxJsonLength=Int32.MaxValue;
        File.WriteAllText(output,json.Serialize(D("dirtyBefore",dirtyBefore,"dirtyAfter",doc.GetSaveFlag(),"parts",parts)),new System.Text.UTF8Encoding(false));
        return "Feature map written: "+output;
    }
    public static string Posts(string path,string modelPath,string output){
        ISldWorks sw=(ISldWorks)Marshal.GetActiveObject("SldWorks.Application");IModelDoc2 doc=(IModelDoc2)sw.GetOpenDocumentByName(path);if(doc==null)throw new Exception("Target not already open.");bool dirtyBefore=doc.GetSaveFlag();IAssemblyDoc assembly=(IAssemblyDoc)doc;var json=new JavaScriptSerializer();json.MaxJsonLength=Int32.MaxValue;var model=(Dictionary<string,object>)json.DeserializeObject(File.ReadAllText(modelPath));var parts=new List<object>();
        foreach(object mr in (object[])model["meshes"]){var mesh=(Dictionary<string,object>)mr;var selected=new HashSet<int>();foreach(object fr in (object[])mesh["faces"]){var f=(Dictionary<string,object>)fr;if((string)f["group"]=="posts"){string sourceId=(string)f["sourceFaceId"];selected.Add(Int32.Parse(sourceId.Substring(sourceId.LastIndexOf("/f")+2)));}}if(selected.Count==0)continue;string name=(string)mesh["name"];IComponent2 c=Find(assembly,name);var faces=new List<object>();object info;int id=0;foreach(object item in (object[])c.GetBodies3(-1,out info)){IBody2 body=(IBody2)item;if(!body.Visible)continue;foreach(object fr in (object[])body.GetFaces()){IFace2 f=(IFace2)fr;int index=id++;if(!selected.Contains(index))continue;IFeature feature=(IFeature)f.GetFeature();ISurface surf=(ISurface)f.GetSurface();faces.Add(D("index",index,"feature",feature==null?null:feature.Name,"type",feature==null?null:feature.GetTypeName2(),"cylinder",surf.IsCylinder()?surf.CylinderParams:null));}}parts.Add(D("name",name,"faces",faces));Console.WriteLine("Read exterior column evidence: "+name);}
        File.WriteAllText(output,json.Serialize(D("dirtyBefore",dirtyBefore,"dirtyAfter",doc.GetSaveFlag(),"parts",parts)),new System.Text.UTF8Encoding(false));return "Exterior post evidence: "+output;
    }
}
