using System;
using System.IO;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Web.Script.Serialization;
using SolidWorks.Interop.sldworks;
public static class CadColorProbe {
    static Dictionary<string,object> D(params object[] values){Dictionary<string,object> d=new Dictionary<string,object>();for(int i=0;i<values.Length;i+=2)d[(string)values[i]]=values[i+1];return d;}
    public static string Run(string path,string output){
        ISldWorks sw=(ISldWorks)Marshal.GetActiveObject("SldWorks.Application");
        IModelDoc2 doc=(IModelDoc2)sw.GetOpenDocumentByName(path); if(doc==null)throw new Exception("Target is not open");
        List<object> parts=new List<object>();
        foreach(object raw in (object[])((IAssemblyDoc)doc).GetComponents(false)){
            IComponent2 c=(IComponent2)raw;string name=c.Name2;
            if(!(name=="打印件_腿部_左腿-1"||name=="打印件_腿部_右腿-1"||name.EndsWith("/打印件_屏幕总成_1-1")))continue;
            Console.WriteLine("Probe face color: "+name);
            IModelDoc2 model=(IModelDoc2)c.GetModelDoc2();
            List<object> bodyList=new List<object>();object info;
            foreach(object br in (object[])c.GetBodies3(-1,out info)){
                IBody2 b=(IBody2)br;List<object> faces=new List<object>();int id=0;
                foreach(object fr in (object[])b.GetFaces()){
                    IFace2 f=(IFace2)fr;IFeature feature=(IFeature)f.GetFeature();
                    faces.Add(D("id",id++,"hasFace",f.HasMaterialPropertyValues(),"face",f.GetMaterialPropertyValues2(1,null),
                        "feature",feature==null?null:feature.Name,"featureColor",feature==null?null:feature.GetMaterialPropertyValues2(1,null)));
                }
                bodyList.Add(D("body",b.Name,"hasBody",b.HasMaterialPropertyValues(),"bodyColor",b.MaterialPropertyValues2,"faces",faces));
            }
            parts.Add(D("name",name,"componentColor",c.GetMaterialPropertyValues2(1,null),"hasComponent",c.HasMaterialPropertyValues(),
                "modelColor",model.MaterialPropertyValues,"modelConfigurationColor",c.GetModelMaterialPropertyValues(c.ReferencedConfiguration),
                "configuration",c.ReferencedConfiguration,"displayState",c.ReferencedDisplayState2,"bodies",bodyList));
        }
        JavaScriptSerializer json=new JavaScriptSerializer();json.MaxJsonLength=Int32.MaxValue;
        File.WriteAllText(output,json.Serialize(parts),new System.Text.UTF8Encoding(false));return "Colors written: "+output;
    }
}
