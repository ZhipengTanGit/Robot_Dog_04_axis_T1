using System;
using System.IO;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Web.Script.Serialization;
using SolidWorks.Interop.sldworks;

public static class CadReadOnlyInspect {
    public static string Run(string assemblyPath, string outputPath) {
        ISldWorks app = (ISldWorks)Marshal.GetActiveObject("SldWorks.Application");
        IModelDoc2 active = (IModelDoc2)app.ActiveDoc;
        IModelDoc2 doc = (IModelDoc2)app.GetOpenDocumentByName(assemblyPath);
        if (doc == null) throw new Exception("Target assembly is not already open; no document was opened or activated.");
        string activeBefore = active == null ? "" : active.GetPathName();
        bool dirtyBefore = doc.GetSaveFlag();
        IAssemblyDoc assembly = (IAssemblyDoc)doc;
        object[] components = (object[])assembly.GetComponents(false);
        List<object> entries = new List<object>();
        foreach (object item in components) {
            IComponent2 c = (IComponent2)item;
            Dictionary<string,object> entry = new Dictionary<string,object>();
            entry["name"] = c.Name2;
            entry["path"] = c.GetPathName();
            entry["suppressed"] = c.IsSuppressed();
            entry["hidden"] = c.IsHidden(false);
            entry["visibility"] = c.Visible;
            entry["suppression"] = c.GetSuppression2();
            IComponent2 parent = (IComponent2)c.GetParent();
            entry["parent"] = parent == null ? null : parent.Name2;
            IModelDoc2 model = (IModelDoc2)c.GetModelDoc2();
            entry["modelLoaded"] = model != null;
            IMathTransform transform = (IMathTransform)c.Transform2;
            entry["transform"] = transform == null ? null : transform.ArrayData;
            entries.Add(entry);
        }
        active = (IModelDoc2)app.ActiveDoc;
        Dictionary<string,object> result = new Dictionary<string,object>();
        result["source"] = assemblyPath;
        result["revision"] = app.RevisionNumber();
        result["activeBefore"] = activeBefore;
        result["activeAfter"] = active == null ? "" : active.GetPathName();
        result["dirtyBefore"] = dirtyBefore;
        result["dirtyAfter"] = doc.GetSaveFlag();
        result["components"] = entries;
        JavaScriptSerializer json = new JavaScriptSerializer();
        json.MaxJsonLength = Int32.MaxValue;
        File.WriteAllText(outputPath,json.Serialize(result),new System.Text.UTF8Encoding(false));
        return "Read-only inventory: " + entries.Count + " components; " + outputPath;
    }
}
