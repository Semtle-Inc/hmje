/* Toolchain-owned, offline Android Java/AAR packaging. Dependency code is never
 * loaded by this host process. Only SDK executables named by the coordinator run. */
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.*;
import javax.xml.XMLConstants;
import javax.xml.parsers.*;
import javax.xml.transform.*;
import javax.xml.transform.dom.DOMSource;
import javax.xml.transform.stream.StreamResult;
import org.w3c.dom.*;

public final class AndroidJvmPackager {
    static final String ANDROID="http://schemas.android.com/apk/res/android", TOOLS="http://schemas.android.com/tools";
    static final long MAX_ARCHIVE=128L<<20, MAX_EXPANDED=512L<<20;
    static final int MAX_ENTRIES=100000;
    static void fail(String s) { throw new IllegalArgumentException("Android Java dependency: "+s); }
    static String read(Path p) throws IOException { return new String(Files.readAllBytes(p),StandardCharsets.UTF_8); }
    static void write(Path p,String s) throws IOException { Files.createDirectories(p.getParent());Files.write(p,s.getBytes(StandardCharsets.UTF_8)); }
    static String sha(Path p) throws Exception { MessageDigest d=MessageDigest.getInstance("SHA-256");try(InputStream in=Files.newInputStream(p)){byte[] b=new byte[65536];int n;while((n=in.read(b))>=0)d.update(b,0,n);}StringBuilder s=new StringBuilder();for(byte b:d.digest())s.append(String.format("%02x",b&255));return s.toString(); }
    static String string(Object o) { if(!(o instanceof String))fail("expected string");return (String)o; }
    @SuppressWarnings("unchecked") static Map<String,Object> object(Object o) { if(!(o instanceof Map))fail("expected object");return (Map<String,Object>)o; }
    @SuppressWarnings("unchecked") static List<Object> array(Object o) { if(!(o instanceof List))fail("expected array");return (List<Object>)o; }
    static void fields(Map<String,Object> map,String...allowed) { Set<String> set=new HashSet<>(Arrays.asList(allowed));for(String s:map.keySet())if(!set.contains(s))fail("unknown lock field "+s); }
    static final class Json {
        final String s;int i,depth;Json(String s){this.s=s;}
        Object parse(){Object v=value();ws();if(i!=s.length())fail("trailing JSON");return v;}
        void ws(){while(i<s.length()&&" \r\n\t".indexOf(s.charAt(i))>=0)i++;}
        char take(){if(i>=s.length())fail("truncated JSON");return s.charAt(i++);}
        Object value(){if(++depth>64)fail("JSON depth limit");try{return innerValue();}finally{depth--;}}
        Object innerValue(){ws();char c=take();if(c=='"')return text();if(c=='{'){Map<String,Object> m=new LinkedHashMap<>();ws();if(i<s.length()&&s.charAt(i)=='}'){i++;return m;}for(;;){ws();if(take()!='"')fail("JSON key");String k=text();ws();if(take()!=':')fail("JSON colon");if(m.containsKey(k))fail("duplicate JSON key "+k);m.put(k,value());ws();char n=take();if(n=='}')return m;if(n!=',')fail("JSON object delimiter");}}
            if(c=='['){List<Object> a=new ArrayList<>();ws();if(i<s.length()&&s.charAt(i)==']'){i++;return a;}for(;;){a.add(value());ws();char n=take();if(n==']')return a;if(n!=',')fail("JSON array delimiter");}}
            if(c>='0'&&c<='9'){int start=i-1;while(i<s.length()&&Character.isDigit(s.charAt(i)))i++;return Long.valueOf(s.substring(start,i));}fail("unsupported JSON value");return null;}
        String text(){StringBuilder b=new StringBuilder();for(;;){char c=take();if(c=='"')return b.toString();if(c<32)fail("JSON control character");if(c!='\\'){b.append(c);continue;}c=take();switch(c){case '"':case '\\':case '/':b.append(c);break;case 'n':b.append('\n');break;case 'r':b.append('\r');break;case 't':b.append('\t');break;case 'b':b.append('\b');break;case 'f':b.append('\f');break;case 'u':if(i+4>s.length())fail("JSON escape");try{b.append((char)Integer.parseInt(s.substring(i,i+4),16));}catch(NumberFormatException e){fail("JSON unicode escape");}i+=4;break;default:fail("JSON escape");}}}
    }
    static final class Artifact {
        String id,type;Path file,dir;List<String> dependencies=new ArrayList<>();
    }
    final Path work;final LinkedHashMap<String,Artifact> artifacts=new LinkedHashMap<>();
    final List<Path> javaSources=new ArrayList<>(),jars=new ArrayList<>(),resources=new ArrayList<>();
    final List<Document> manifests=new ArrayList<>();final LinkedHashSet<String> packages=new LinkedHashSet<>();
    final Map<String,String> classes=new HashMap<>();final Map<String,byte[]> payload=new TreeMap<>();
    long expanded;int entries;String minD8="0.0.0";
    AndroidJvmPackager(Path work){this.work=work;}
    void plan(Path plan) throws Exception {
        for(String line:Files.readAllLines(plan,StandardCharsets.UTF_8)){
            String[] p=line.split("\t",-1);if(p.length!=2||!p[0].equals("source"))continue;
            Path source=Paths.get(p[1]).toRealPath();String name=source.getFileName().toString();
            if(name.endsWith(".java"))javaSources.add(source);
            else if(name.endsWith(".android.lock.json"))lock(source);
            else if(name.endsWith(".jar")||name.endsWith(".aar"))fail("JAR/AAR must be declared in a checksum lock: "+source);
        }
        for(Artifact a:artifacts.values())for(String d:a.dependencies)if(!artifacts.containsKey(d))fail("missing locked dependency "+d+" required by "+a.id);
        List<Artifact> order=new ArrayList<>();Set<String> done=new HashSet<>();for(Artifact a:artifacts.values())visit(a,new HashSet<>(),done,order);
        int index=0;for(Artifact a:order){a.dir=work.resolve("archives/"+(index++));if(a.type.equals("aar"))aar(a);else if(a.type.equals("jar")){jars.add(a.file);inspectJar(a.file,a.id);}}
    }
    void lock(Path file) throws Exception {
        if(Files.size(file)>4L<<20)fail("lock file too large");Map<String,Object> root=object(new Json(read(file)).parse());fields(root,"version","roots","artifacts","minD8Version");if(!Long.valueOf(1).equals(root.get("version")))fail("unsupported lock version");
        if(root.containsKey("minD8Version")){String v=string(root.get("minD8Version"));if(!v.matches("[0-9]+\\.[0-9]+\\.[0-9]+"))fail("invalid minD8Version");if(compareVersion(v,minD8)>0)minD8=v;}
        Set<String> inLock=new HashSet<>();for(Object value:array(root.get("artifacts"))){Map<String,Object> row=object(value);fields(row,"id","path","type","sha256","dependencies","url");Artifact a=new Artifact();a.id=string(row.get("id"));if(!a.id.matches("[A-Za-z0-9_.-]+:[A-Za-z0-9_.-]+:[A-Za-z0-9_.-]+"))fail("coordinate must have an exact version: "+a.id);a.type=string(row.get("type"));if(!a.type.equals("jar")&&!a.type.equals("aar")&&!a.type.equals("pom"))fail("archive type "+a.type);
            String relative=string(row.get("path"));safeName(relative);a.file=file.getParent().resolve(relative).toRealPath();if(!a.file.startsWith(file.getParent().toRealPath()))fail("lock path escapes its directory");if(Files.size(a.file)>MAX_ARCHIVE)fail("archive too large "+a.id);
            String checksum=string(row.get("sha256"));if(!checksum.matches("[0-9a-f]{64}")||!sha(a.file).equals(checksum))fail("checksum mismatch "+a.id);
            for(Object d:array(row.get("dependencies")))a.dependencies.add(string(d));if(!inLock.add(a.id))fail("duplicate coordinate "+a.id);
            for(Artifact existing:artifacts.values())if(!existing.id.equals(a.id)&&existing.id.substring(0,existing.id.lastIndexOf(':')).equals(a.id.substring(0,a.id.lastIndexOf(':'))))fail("multiple locked versions of "+a.id);
            Artifact old=artifacts.get(a.id);if(old!=null){if(!sha(old.file).equals(checksum)||!old.type.equals(a.type)||!old.dependencies.equals(a.dependencies))fail("conflicting coordinate "+a.id);}else artifacts.put(a.id,a);
        }
        if(array(root.get("roots")).isEmpty())fail("lock has no roots");
        for(Object r:array(root.get("roots")))if(!inLock.contains(string(r)))fail("root absent from lock "+r);
        // The final graph is closed and all inputs are explicit; no URLs are opened.
    }
    void visit(Artifact a,Set<String> path,Set<String> done,List<Artifact> order){if(done.contains(a.id))return;if(!path.add(a.id))fail("dependency cycle "+a.id);for(String d:a.dependencies)visit(artifacts.get(d),path,done,order);path.remove(a.id);done.add(a.id);order.add(a);}
    static void safeName(String s){for(int i=0;i<s.length();i++)if(s.charAt(i)<32)fail("control character in path");if(s.isEmpty()||s.startsWith("/")||s.indexOf('\\')>=0||s.indexOf('\0')>=0||s.indexOf(':')>=0)fail("unsafe archive/path name "+s);String[] parts=s.split("/",-1);for(int i=0;i<parts.length;i++)if(parts[i].equals("..")||parts[i].equals(".")||(parts[i].isEmpty()&&i<parts.length-1))fail("path traversal/empty component "+s);}
    byte[] entry(ZipFile zip,ZipEntry e) throws Exception {if(++entries>MAX_ENTRIES)fail("too many ZIP entries");if(e.getSize()>MAX_ARCHIVE)fail("oversized ZIP entry "+e.getName());try(InputStream in=zip.getInputStream(e);ByteArrayOutputStream out=new ByteArrayOutputStream()){byte[] b=new byte[65536];int n;while((n=in.read(b))>=0){expanded+=n;if(expanded>MAX_EXPANDED||out.size()+n>MAX_ARCHIVE)fail("expanded archive budget exceeded");out.write(b,0,n);}byte[] bytes=out.toByteArray();CRC32 crc=new CRC32();crc.update(bytes);if(e.getCrc()>=0&&crc.getValue()!=e.getCrc())fail("ZIP CRC mismatch "+e.getName());return bytes;}}
    void put(String name,byte[] bytes) throws IOException {safeName(name);byte[] old=payload.get(name);if(old==null){payload.put(name,bytes);return;}if(Arrays.equals(old,bytes))return;if((name.startsWith("META-INF/com.android.tools/proguard/")||name.startsWith("META-INF/proguard/"))){byte[] joined=(new String(old,StandardCharsets.UTF_8)+"\n"+new String(bytes,StandardCharsets.UTF_8)).getBytes(StandardCharsets.UTF_8);payload.put(name,joined);return;}if(name.startsWith("META-INF/services/")){LinkedHashSet<String> lines=new LinkedHashSet<>();for(byte[] v:new byte[][]{old,bytes})for(String s:new String(v,StandardCharsets.UTF_8).split("\n"))if(!s.trim().isEmpty())lines.add(s.trim());payload.put(name,(String.join("\n",lines)+"\n").getBytes(StandardCharsets.UTF_8));return;}fail("conflicting packaged resource "+name);}
    void inspectJar(Path jar,String owner) throws Exception {Set<String> names=new HashSet<>();try(ZipFile zip=new ZipFile(jar.toFile())){Enumeration<? extends ZipEntry> es=zip.entries();while(es.hasMoreElements()){ZipEntry e=es.nextElement();String name=e.getName();safeName(name);if(!names.add(name))fail("duplicate ZIP entry "+name+" in "+owner);if(e.isDirectory()){if(++entries>MAX_ENTRIES)fail("too many ZIP entries");continue;}
            if(name.endsWith(".class")){entry(zip,e);if(name.equals("module-info.class")||name.startsWith("META-INF/versions/"))continue;String previous=classes.putIfAbsent(name,owner);if(previous!=null)fail("duplicate class "+name+" from "+previous+" and "+owner);}
            else if(name.matches("(?i)META-INF/(LICENSE|NOTICE|COPYING).*"))put("META-INF/hmje-notices/"+owner.replace(':','_')+"/"+name.substring(9),entry(zip,e));
            else if(!name.equals("META-INF/MANIFEST.MF")&&!name.matches("META-INF/[^/]+\\.(SF|RSA|DSA|EC)")&&!name.startsWith("META-INF/versions/"))put(name,entry(zip,e));
        }}}
    void aar(Artifact a) throws Exception {Set<String> names=new HashSet<>(),portableNames=new HashSet<>(),nativeNames=new HashSet<>(),arm64Names=new HashSet<>();try(ZipFile zip=new ZipFile(a.file.toFile())){Enumeration<? extends ZipEntry> es=zip.entries();while(es.hasMoreElements()){ZipEntry e=es.nextElement();String name=e.getName();safeName(name);if(!names.add(name))fail("duplicate ZIP entry "+name+" in "+a.id);if(!portableNames.add(java.text.Normalizer.normalize(name,java.text.Normalizer.Form.NFC).toLowerCase(Locale.ROOT)))fail("nonportable colliding ZIP entry "+name+" in "+a.id);if(e.isDirectory()){if(++entries>MAX_ENTRIES)fail("too many ZIP entries");continue;}byte[] b=entry(zip,e);
            if(name.startsWith("jni/")&&name.endsWith(".so"))nativeNames.add(name.substring(name.lastIndexOf('/')+1));
            if(name.equals("AndroidManifest.xml")||name.equals("classes.jar")||name.startsWith("libs/")&&name.endsWith(".jar")||name.startsWith("res/")||name.equals("META-INF/com/android/build/gradle/aar-metadata.properties")){Path p=a.dir.resolve(name);Files.createDirectories(p.getParent());Files.write(p,b);}
            else if(name.startsWith("assets/"))put(name,b);
            else if(name.startsWith("jni/arm64-v8a/")&&name.endsWith(".so")){if(b.length<20||b[0]!=127||b[1]!='E'||b[2]!='L'||b[3]!='F'||b[4]!=2||b[5]!=1||(b[18]&255)!=183||b[19]!=0)fail("invalid arm64 native library "+name);arm64Names.add(name.substring(name.lastIndexOf('/')+1));put("lib/arm64-v8a/"+name.substring("jni/arm64-v8a/".length()),b);}
            else if(name.startsWith("prefab/"))fail("AAR prefab requires explicit native linking: "+a.id);
            else if(name.equals("proguard.txt")||name.equals("consumer-rules.pro"))put("META-INF/hmje-consumer-rules/"+a.id.replace(':','_')+"-"+name,b);
            else if(name.equals("baseline-prof.txt"))put("META-INF/hmje-consumer-profiles/"+a.id.replace(':','_')+".txt",b);
            else if(name.startsWith("third_party_licenses.")||name.matches("(?i).*[/]?(LICENSE|NOTICE|COPYING)[^/]*"))put("META-INF/hmje-notices/"+a.id.replace(':','_')+"/"+name,b);
        }}
        if(!arm64Names.containsAll(nativeNames))fail("AAR native libraries lack arm64-v8a counterparts: "+a.id);
        Path manifest=a.dir.resolve("AndroidManifest.xml");if(!Files.exists(manifest))fail("AAR manifest missing "+a.id);Document xml=xml(manifest);manifests.add(xml);String pkg=xml.getDocumentElement().getAttribute("package");if(!pkg.matches("[A-Za-z_]\\w*(\\.[A-Za-z_]\\w*)+"))fail("invalid AAR package "+a.id);packages.add(pkg);
        Path res=a.dir.resolve("res");if(Files.isDirectory(res))resources.add(res);
        List<Path> found=new ArrayList<>();try(java.util.stream.Stream<Path> stream=Files.walk(a.dir)){stream.filter(p->p.toString().endsWith(".jar")).forEach(found::add);}Collections.sort(found);for(Path jar:found){jars.add(jar);inspectJar(jar,a.id);}
    }
    static Document xml(Path p) throws Exception {DocumentBuilderFactory f=DocumentBuilderFactory.newInstance();f.setNamespaceAware(true);f.setFeature("http://apache.org/xml/features/disallow-doctype-decl",true);f.setFeature("http://xml.org/sax/features/external-general-entities",false);f.setFeature("http://xml.org/sax/features/external-parameter-entities",false);f.setAttribute(XMLConstants.ACCESS_EXTERNAL_DTD,"");f.setAttribute(XMLConstants.ACCESS_EXTERNAL_SCHEMA,"");return f.newDocumentBuilder().parse(p.toFile());}
    static void xmlWrite(Document xml,Path p) throws Exception {TransformerFactory f=TransformerFactory.newInstance();f.setAttribute(XMLConstants.ACCESS_EXTERNAL_DTD,"");f.setAttribute(XMLConstants.ACCESS_EXTERNAL_STYLESHEET,"");Transformer t=f.newTransformer();t.setOutputProperty(OutputKeys.INDENT,"yes");t.transform(new DOMSource(xml),new StreamResult(p.toFile()));}
    static List<Element> children(Element parent){List<Element> out=new ArrayList<>();for(Node n=parent.getFirstChild();n!=null;n=n.getNextSibling())if(n instanceof Element)out.add((Element)n);return out;}
    static Element child(Element p,String name){for(Element e:children(p))if(e.getTagName().equals(name))return e;return null;}
    static String attr(Element e,String name){return e.getAttributeNS(ANDROID,name);}
    static int number(String s,String label){try{return Integer.parseInt(s);}catch(NumberFormatException e){fail("non-numeric "+label+": "+s);return 0;}}
    static String component(String value,String pkg){if(value.isEmpty())return value;if(value.startsWith("."))return pkg+value;if(value.indexOf('.')<0)return pkg+"."+value;return value;}
    static boolean componentTag(String tag){return Arrays.asList("activity","activity-alias","service","receiver","provider","application","instrumentation").contains(tag);}
    static void normalize(Element e,String pkg,String appId){
        NamedNodeMap attributes=e.getAttributes();List<Attr> remove=new ArrayList<>();
        for(int i=0;i<attributes.getLength();i++){
            Attr a=(Attr)attributes.item(i);String value=a.getValue();
            if(TOOLS.equals(a.getNamespaceURI())){
                if(!a.getLocalName().equals("ignore")&&!(a.getLocalName().equals("node")&&value.equals("merge")))fail("unsupported manifest directive tools:"+a.getLocalName()+"="+value);
                remove.add(a);continue;
            }
            value=value.replace("${applicationId}",appId);
            if(value.contains("${"))fail("unresolved manifest placeholder "+value);
            if(ANDROID.equals(a.getNamespaceURI())&&((a.getLocalName().equals("name")&&componentTag(e.getTagName()))||a.getLocalName().equals("targetActivity")||a.getLocalName().equals("parentActivityName")||a.getLocalName().equals("appComponentFactory")))value=component(value,pkg);
            a.setValue(value);
        }
        for(Attr a:remove)e.removeAttributeNode(a);
        for(Element c:children(e))normalize(c,pkg,appId);
    }
    static String signature(Element e){
        List<String> a=new ArrayList<>();NamedNodeMap attrs=e.getAttributes();
        for(int i=0;i<attrs.getLength();i++){Node n=attrs.item(i);if(!n.getNodeName().startsWith("xmlns"))a.add(n.getNamespaceURI()+":"+n.getLocalName()+"="+n.getNodeValue());}
        Collections.sort(a);List<String> c=new ArrayList<>();for(Element n:children(e))c.add(signature(n));Collections.sort(c);
        return e.getTagName()+a+c;
    }
    static String identity(Element e){
        String tag=e.getTagName();
        if(tag.equals("application")||tag.equals("queries")||tag.equals("supports-screens"))return tag;
        String name=attr(e,"name");if(!name.isEmpty()&&!tag.equals("intent-filter"))return tag+":"+name;
        return signature(e);
    }
    static void merge(Element target,Element source,Set<String> appOverrides,String path){
        NamedNodeMap attrs=source.getAttributes();
        for(int i=0;i<attrs.getLength();i++){
            Attr a=(Attr)attrs.item(i);if(a.getName().startsWith("xmlns")||a.getName().equals("package"))continue;
            String ns=a.getNamespaceURI(),local=a.getLocalName();if(local==null)local=a.getName();
            Attr old=target.getAttributeNodeNS(ns,local);String key=path+"/@"+ns+":"+local;
            if(old==null)target.setAttributeNS(ns,a.getName(),a.getValue());
            else if(!old.getValue().equals(a.getValue())&&!appOverrides.contains(key))fail("manifest attribute conflict "+key+": "+old.getValue()+" vs "+a.getValue());
        }
        for(Element next:children(source)){
            String id=identity(next);Element matching=null;
            for(Element existing:children(target))if(identity(existing).equals(id)){matching=existing;break;}
            if(matching==null)target.appendChild(target.getOwnerDocument().importNode(next,true));
            else merge(matching,next,appOverrides,path+"/"+id);
        }
    }
    static void overrides(Element e,String path,Set<String> set){
        NamedNodeMap attrs=e.getAttributes();for(int i=0;i<attrs.getLength();i++){Attr a=(Attr)attrs.item(i);set.add(path+"/@"+a.getNamespaceURI()+":"+(a.getLocalName()==null?a.getName():a.getLocalName()));}
        for(Element c:children(e))overrides(c,path+"/"+identity(c),set);
    }
    Path manifest(Path source,int minSdk,int targetSdk,int compileSdk) throws Exception {
        Document out=xml(source);Element appRoot=out.getDocumentElement();String appId=appRoot.getAttribute("package");
        if(!appRoot.getTagName().equals("manifest")||!appId.matches("[A-Za-z_]\\w*(\\.[A-Za-z_]\\w*)+"))fail("invalid application manifest package");
        normalize(appRoot,appId,appId);Set<String> appOverrides=new HashSet<>();overrides(appRoot,"manifest",appOverrides);
        for(Document d:manifests){
            Element root=d.getDocumentElement();normalize(root,root.getAttribute("package"),appId);
            Element sdk=child(root,"uses-sdk");if(sdk!=null){String v=attr(sdk,"minSdkVersion");if(!v.isEmpty()&&number(v,"library minSdk")>minSdk)fail("library minSdk "+v+" exceeds application "+minSdk);root.removeChild(sdk);}
            // Library version/compile SDK metadata never replace the app's identity.
            for(String s:new String[]{"versionCode","versionName","compileSdkVersion","compileSdkVersionCodename"})root.removeAttributeNS(ANDROID,s);
            root.removeAttribute("platformBuildVersionCode");root.removeAttribute("platformBuildVersionName");
            merge(appRoot,root,appOverrides,"manifest");
        }
        Element sdk=child(appRoot,"uses-sdk");if(sdk==null){sdk=out.createElement("uses-sdk");appRoot.insertBefore(sdk,appRoot.getFirstChild());}
        sdk.setAttributeNS(ANDROID,"android:minSdkVersion",Integer.toString(minSdk));sdk.setAttributeNS(ANDROID,"android:targetSdkVersion",Integer.toString(targetSdk));
        Element application=child(appRoot,"application");if(application==null)fail("application missing from manifest");
        for(Element c:children(application))if(targetSdk>=31&&Arrays.asList("activity","activity-alias","service","receiver").contains(c.getTagName())&&child(c,"intent-filter")!=null&&!attr(c,"exported").matches("true|false"))fail("component with intent-filter needs explicit android:exported: "+attr(c,"name"));
        for(Artifact a:artifacts.values())if(a.type.equals("aar")){
            Path p=a.dir.resolve("META-INF/com/android/build/gradle/aar-metadata.properties");if(!Files.exists(p))continue;
            Properties m=new Properties();try(Reader r=Files.newBufferedReader(p)){m.load(r);}
            for(String f:new String[]{"aarFormatVersion","aarMetadataVersion"})if(!m.getProperty(f,"1.0").equals("1.0"))fail("unsupported "+f+" "+a.id);
            if(number(m.getProperty("minCompileSdk","1"),"minCompileSdk")>compileSdk)fail("minCompileSdk exceeds platform android.jar: "+a.id);
            if(number(m.getProperty("minCompileSdkExtension","0"),"minCompileSdkExtension")>0)fail("SDK extension requirement needs an explicitly supported platform: "+a.id);
            if(Boolean.parseBoolean(m.getProperty("coreLibraryDesugaringEnabled","false")))fail("core library desugaring is not declared: "+a.id);
        }
        Path p=work.resolve("AndroidManifest.xml");xmlWrite(out,p);return p;
    }
    static List<String> sdkCommand(List<String> command) {
        String tool=command.get(0);
        if(System.getProperty("os.name").startsWith("Windows") && tool.equals("javac")) {
            List<String> result=new ArrayList<>(command);
            result.set(0,Paths.get(System.getProperty("java.home"),"bin","javac.exe").toString());return result;
        }
        if(System.getProperty("os.name").startsWith("Windows") && tool.toLowerCase(Locale.ROOT).endsWith("d8.bat")) {
            Path jar=Paths.get(tool).toAbsolutePath().getParent().resolve("lib/d8.jar");
            List<String> result=new ArrayList<>(Arrays.asList(Paths.get(System.getProperty("java.home"),"bin","java.exe").toString(),"-cp",jar.toString(),"com.android.tools.r8.D8"));
            result.addAll(command.subList(1,command.size()));return result;
        }
        return command;
    }
    static void run(List<String> command) throws Exception {
        Process p=new ProcessBuilder(sdkCommand(command)).inheritIO().start();int status=p.waitFor();if(status!=0)fail("SDK tool failed ("+status+"): "+command.get(0));
    }
    static int compareVersion(String a,String b){String[] aa=a.split("\\."),bb=b.split("\\.");for(int i=0;i<3;i++){int c=Integer.compare(number(aa[i],"tool version"),number(bb[i],"tool version"));if(c!=0)return c;}return 0;}
    void checkD8(String d8) throws Exception {
        if(minD8.equals("0.0.0"))return;
        Process p=new ProcessBuilder(sdkCommand(Arrays.asList(d8,"--version"))).redirectErrorStream(true).start();ByteArrayOutputStream out=new ByteArrayOutputStream();byte[] b=new byte[1024];int n;try(InputStream in=p.getInputStream()){while((n=in.read(b))>=0){if(out.size()+n>65536)fail("oversized D8 version response");out.write(b,0,n);}}
        String v=new String(out.toByteArray(),StandardCharsets.UTF_8);java.util.regex.Matcher m=java.util.regex.Pattern.compile("D8 ([0-9]+\\.[0-9]+\\.[0-9]+)([^0-9]|$)").matcher(v);
        if(p.waitFor()!=0||!m.find()||compareVersion(m.group(1),minD8)<0)fail("D8 >= "+minD8+" required; configure HMJE_ANDROID_D8_OVERRIDE with a compatible installed SDK tool");
    }
    static List<Path> files(Path dir,String suffix) throws IOException {
        List<Path> result=new ArrayList<>();if(Files.exists(dir))try(java.util.stream.Stream<Path> s=Files.walk(dir)){s.filter(p->Files.isRegularFile(p)&&p.toString().endsWith(suffix)).forEach(result::add);}Collections.sort(result);return result;
    }
    static void archive(Path destination,Map<String,byte[]> entries) throws IOException {
        try(ZipOutputStream out=new ZipOutputStream(Files.newOutputStream(destination))){for(Map.Entry<String,byte[]> e:entries.entrySet()){ZipEntry z=new ZipEntry(e.getKey());z.setTime(0);if(e.getKey().equals("resources.arsc")){CRC32 crc=new CRC32();crc.update(e.getValue());z.setMethod(ZipEntry.STORED);z.setSize(e.getValue().length);z.setCompressedSize(e.getValue().length);z.setCrc(crc.getValue());}out.putNextEntry(z);out.write(e.getValue());out.closeEntry();}}
    }
    void build(Path plan,Path manifest,Path platform,String aapt2,String d8,int minSdk,int targetSdk,List<Path> appJava) throws Exception {
        int compileSdk=number(platform.getParent().getFileName().toString().replaceFirst("^android-",""),"platform API");
        plan(plan);checkD8(d8);Path merged=manifest(manifest,minSdk,targetSdk,compileSdk),generated=work.resolve("generated"),classesDir=work.resolve("classes"),dex=work.getParent().resolve("dex");
        // The coordinator's own res/ (next to the manifest) carries app resources such as
        // the FileProvider path list; link it with the dependency resources so manifest
        // references like @xml/hmje_file_paths resolve.
        Path appRes=manifest.toAbsolutePath().getParent().resolve("res");
        if(Files.isDirectory(appRes)){try(java.util.stream.Stream<Path> walk=Files.walk(appRes)){if(walk.anyMatch(Files::isRegularFile))resources.add(appRes);}}
        Files.createDirectories(generated);Files.createDirectories(classesDir);Files.createDirectories(dex);
        List<String> link=new ArrayList<>(Arrays.asList(aapt2,"link","-o",work.resolve("resources.apk").toString(),"-I",platform.toString(),"--manifest",merged.toString(),"--java",generated.toString(),"--auto-add-overlay","--non-final-ids"));
        if(!packages.isEmpty()){link.add("--extra-packages");link.add(String.join(":",packages));}
        int index=0;for(Path res:resources){Path compiled=work.resolve("res-"+(index++)+".zip");run(Arrays.asList(aapt2,"compile","--dir",res.toString(),"-o",compiled.toString()));link.add("-R");link.add(compiled.toString());}
        run(link);
        List<String> classpath=new ArrayList<>();classpath.add(platform.toString());for(Path jar:jars)classpath.add(jar.toString());
        Path lambdaStubs=Paths.get(aapt2).toAbsolutePath().getParent().resolve("core-lambda-stubs.jar");
        if(!Files.isRegularFile(lambdaStubs))fail("SDK core-lambda-stubs.jar is missing beside aapt2");
        String bootclasspath=platform+File.pathSeparator+lambdaStubs;
        List<String> javac=new ArrayList<>(Arrays.asList("javac","-source","8","-target","8","-proc:none","-encoding","UTF-8","-bootclasspath",bootclasspath,"-classpath",String.join(File.pathSeparator,classpath),"-d",classesDir.toString()));
        for(Path java:appJava)javac.add(java.toString());for(Path java:javaSources)javac.add(java.toString());for(Path java:files(generated,".java"))javac.add(java.toString());run(javac);
        Map<String,byte[]> compiledClasses=new TreeMap<>();for(Path f:files(classesDir,".class")){String name=classesDir.relativize(f).toString().replace(File.separatorChar,'/');if(classes.containsKey(name))fail("application duplicate class "+name+" also in "+classes.get(name));compiledClasses.put(name,Files.readAllBytes(f));}
        Path appJar=work.resolve("application.jar");archive(appJar,compiledClasses);
        List<String> dexCommand=new ArrayList<>(Arrays.asList(d8,"--output",dex.toString(),"--min-api",Integer.toString(minSdk),"--lib",platform.toString(),appJar.toString()));for(Path jar:jars)dexCommand.add(jar.toString());run(dexCommand);
        // App assets are copied later by the coordinator. Never let zip update hide a library conflict.
        for(String line:Files.readAllLines(plan,StandardCharsets.UTF_8)){String[] p=line.split("\t",-1);if(p.length==3&&p[0].equals("resource")){String name="assets/"+p[2];safeName(name);byte[] old=payload.get(name);if(old!=null&&!Arrays.equals(old,Files.readAllBytes(Paths.get(p[1]))))fail("application asset conflicts with dependency: "+name);}}
        if(payload.containsKey("lib/arm64-v8a/libhmjeapp.so"))fail("dependency reserves application native library libhmjeapp.so");
        try(ZipFile zip=new ZipFile(work.resolve("resources.apk").toFile())){Enumeration<? extends ZipEntry> es=zip.entries();while(es.hasMoreElements()){ZipEntry e=es.nextElement();if(!e.isDirectory())put(e.getName(),entry(zip,e));}}
        for(Path f:files(dex,".dex"))put(f.getFileName().toString(),Files.readAllBytes(f));
        archive(work.getParent().resolve("unsigned-base.apk"),payload);
        write(work.resolve("inputs.sha256"),receipt());
        System.out.println("Android JVM: "+artifacts.size()+" locked artifacts, "+javaSources.size()+" declared Java sources, "+classes.size()+" dependency classes");
    }
    String receipt() throws Exception {StringBuilder out=new StringBuilder();for(Artifact a:artifacts.values())out.append(sha(a.file)).append("  ").append(a.id).append('\n');for(Path p:javaSources)out.append(sha(p)).append("  ").append(p).append('\n');return out.toString();}
    public static void main(String[] args) throws Exception {
        try{
            if(args.length<3)fail("usage: --build plan work manifest android.jar aapt2 d8 minSdk targetSdk app.java... | --validate plan work manifest compileSdk minSdk targetSdk");
            Path plan=Paths.get(args[1]),work=Paths.get(args[2]);Files.createDirectories(work);try(DirectoryStream<Path> present=Files.newDirectoryStream(work)){if(present.iterator().hasNext())fail("packager work directory must be empty");}AndroidJvmPackager pack=new AndroidJvmPackager(work);
            if(args[0].equals("--validate")&&args.length==7){pack.plan(plan);pack.manifest(Paths.get(args[3]),number(args[5],"minSdk"),number(args[6],"targetSdk"),number(args[4],"compileSdk"));System.out.println("Android JVM validated: "+pack.artifacts.size());return;}
            if(!args[0].equals("--build")||args.length<10)fail("invalid packager arguments");
            List<Path> java=new ArrayList<>();for(int i=9;i<args.length;i++)java.add(Paths.get(args[i]));
            pack.build(plan,Paths.get(args[3]),Paths.get(args[4]),args[5],args[6],number(args[7],"minSdk"),number(args[8],"targetSdk"),java);
        }catch(IllegalArgumentException e){System.err.println(e.getMessage());System.exit(2);}
    }
}
