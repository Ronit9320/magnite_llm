"""Generate editable, self-contained Magnite design SVGs (Python stdlib only)."""
from pathlib import Path
from html import escape
import random
import json
import shutil

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
C = dict(bg='#030914', deep='#071222', card='#0C1B30', raised='#122640', border='#243D5E', blue='#2563EB', ice='#93C5FD', white='#FFFFFF', text='#B3C7E1', muted='#7B96B9')
NAMES = ['Models', 'Training', 'RAG', 'Adapters', 'Chat', 'Settings']
ICONS = ['box', 'graduation-cap', 'database', 'layers', 'message-circle', 'settings']

class SVG:
    def __init__(self, name):
        self.name, self.parts = name, []
        self.add(f'<svg xmlns="http://www.w3.org/2000/svg" width="1440" height="960" viewBox="0 0 1440 960"><title>Magnite — {name}</title><desc>Midnight desktop concept. Illustrative data. Editable vector shapes and SVG text.</desc>')
    def add(self, s): self.parts.append(s)
    def group(self, name): self.add(f'<g id="{escape(name, quote=True)}">')
    def end(self): self.add('</g>')
    def rect(self,x,y,w,h,fill='card',r=12,stroke=None,opacity=1):
        self.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{C.get(fill,fill)}" opacity="{opacity}"'+(f' stroke="{C.get(stroke,stroke)}"' if stroke else '')+'/>')
    def text(self,x,y,t,size=14,color='white',weight=400,font='DM Sans',anchor='start'):
        self.add(f'<text x="{x}" y="{y}" fill="{C[color]}" font-family="{font}, sans-serif" font-size="{size}" font-weight="{weight}" text-anchor="{anchor}">{escape(str(t))}</text>')
    def heading(self,x,y,t,size=20): self.text(x,y,t,size,weight=600,font='Space Grotesk')
    def line(self,x,y,w): self.rect(x,y,w,1,'border',0)
    def icon(self,x,y,name,color='text',size=20):
        src=(ROOT/'app/resources/icons'/f'{name}.svg').read_text()
        body=src[src.index('>')+1:src.rindex('</svg>')]
        self.add(f'<g transform="translate({x} {y}) scale({size/24})" fill="none" stroke="{C[color]}" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">{body}</g>')
    def button(self,x,y,w,label,primary=False,icon=None,disabled=False):
        if disabled:self.add('<g opacity="0.4">')
        self.rect(x,y,w,42,'blue' if primary else 'raised',8,None if primary else 'border')
        if icon:self.icon(x+14,y+12,icon,'white',18)
        self.text(x+w/2+(10 if icon else 0),y+26,label,14,weight=500,anchor='middle')
        if disabled:self.end()
    def badge(self,x,y,label,w=None):
        w=w or len(label)*7+24
        self.rect(x,y,w,26,'raised',13,'border');self.text(x+12,y+18,label,12,'ice')
    def field(self,x,y,w,label,value,dropdown=False):
        self.text(x,y,label,12,'text');self.rect(x,y+12,w,44,'deep',8,'border');self.text(x+14,y+39,value)
        if dropdown:self.add(f'<path d="m{x+w-28} {y+31} 5 5 5-5" fill="none" stroke="{C["text"]}" stroke-width="1.5"/>')
    def card(self,x,y,w,h,title=None):
        self.rect(x,y,w,h,'card',12,'border')
        if title:self.heading(x+24,y+38,title)
    def toggle(self,x,y,on=True):
        self.rect(x,y,36,20,'blue' if on else 'border',10)
        self.add(f'<circle cx="{x+(26 if on else 10)}" cy="{y+10}" r="6" fill="{C["white"]}"/>')
    def progress(self,x,y,w,value):
        self.rect(x,y,w,5,'raised',2.5);self.rect(x,y,w*value,5,'blue',2.5)
    def shell(self,subtitle):
        self.rect(0,0,1440,960,'bg',0)
        self.group('Starfield')
        rng=random.Random(24)
        for i in range(145):
            x,y=rng.uniform(248,1432),rng.uniform(60,950)
            self.add(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{rng.choice([.65,.8,1,1.3])}" fill="#FFFFFF" opacity="{rng.uniform(.15,.6):.2f}"/>')
        self.end()
        self.group('Application-shell')
        self.rect(0,0,240,960,'deep',0);self.rect(240,0,1200,56,'deep',0)
        self.line(240,55,1200)
        self.text(272,34,'WORKSPACE',11,'muted',500);self.text(371,34,'/  Local',12,'text')
        self.add(f'<circle cx="1226" cy="29" r="3" fill="{C["ice"]}"/>')
        self.text(1238,34,'Backend connected',12,'text')
        self.rect(1374,27,10,1,'text',0);self.rect(1400,24,8,8,'deep',1,'text')
        self.add(f'<g transform="translate(28 27)" fill="none" stroke="{C["ice"]}" stroke-width="1.8"><ellipse cx="15" cy="15" rx="17" ry="7" transform="rotate(-35 15 15)"/><circle cx="15" cy="15" r="10"/><circle cx="15" cy="15" r="2" fill="{C["white"]}"/></g>')
        self.heading(72,49,'magnite',24);self.text(28,100,'YOUR AI WORKSPACE',10,'muted',500)
        for i,(name,icon) in enumerate(zip(NAMES,ICONS)):
            y=126+i*58 if i<5 else 782
            active=name==self.name
            if active:
                self.rect(16,y,208,46,'raised',8);self.rect(16,y+11,3,24,'ice',1)
            self.icon(32,y+13,icon,'ice' if active else 'text')
            self.text(68,y+29,name,14,'white' if active else 'text',500 if active else 400)
            if active:self.text(203,y+29,'›',20,'ice')
        self.line(24,851,192);self.text(28,884,'LOCAL BY DEFAULT',10,'ice',500)
        self.text(28,910,'Your models. Your machine.',12,'text');self.text(28,936,'MIDNIGHT  /  01',10,'muted')
        self.heading(272,112,self.name if self.name!='RAG' else 'Knowledge & retrieval',28)
        self.text(272,143,subtitle,14,'text')
        self.text(1408,932,'MAGNITE  /  LOCAL WORKSPACE',10,'muted',anchor='end')
        self.end()
    def save(self,index):
        p=HERE/'panels'/f'{index:02d}-{self.name.lower()}.svg'
        p.write_text('\n'.join(self.parts+['</svg>']))
        return p

def models():
    s=SVG('Models');s.shell('Find your next model. Keep it close.')
    s.button(1264,90,144,'Refresh',icon='refresh-cw')
    s.group('Model-discovery')
    s.rect(272,180,916,48,'deep',8,'border');s.icon(288,194,'search');s.text(321,210,'Search Hugging Face models…',14,'text')
    s.button(1204,183,204,'Search models',True)
    s.text(272,274,'Local models',14,'white',500);s.text(416,274,'Hub search results',14,'text');s.rect(272,287,104,2,'ice',0)
    s.text(1408,274,'3 downloaded · 1 in progress',12,'text',anchor='end');s.end()
    data=[('Llama 3.2 1B Instruct','meta-llama / Llama-3.2-1B-Instruct','1.3 GB','Llama','Ready locally','Load model'),('Qwen 2.5 3B Instruct','Qwen / Qwen2.5-3B-Instruct','2.1 GB','Qwen2','Ready locally','Load model'),('Phi 3 Mini','microsoft / Phi-3-mini-4k-instruct','2.4 GB','Phi3','Ready locally','Load model'),('Mistral 7B Instruct','mistralai / Mistral-7B-Instruct-v0.3','4.4 GB','Mistral','Downloading · 64%','Cancel download')]
    for i,(title,repo,size,arch,status,action) in enumerate(data):
        x,y=272+(i%2)*580,316+(i//2)*248;s.group(f'Model-card-{i+1}');s.card(x,y,556,224)
        s.icon(x+24,y+24,'box','ice',24);s.badge(x+428,y+24,arch,104)
        s.heading(x+24,y+84,title);s.text(x+24,y+111,repo,12,'text');s.text(x+24,y+145,f'Hugging Face   ·   {size}   ·   GGUF',12,'text')
        if i==3:s.progress(x+24,y+161,508,.64)
        s.text(x+24,y+194,('↓  ' if i==3 else '✓  ')+status,12,'ice')
        s.button(x+354,y+170,178,action,i==0)
        s.end()
    s.card(272,836,1136,64);s.icon(296,858,'folder','ice');s.text(332,874,'Model storage',14,weight=500);s.text(464,874,'~/.magnite/models',13,'text');s.text(1384,874,'6.0 GB downloaded',12,'text',anchor='end')
    return s

def training():
    s=SVG('Training');s.shell('Turn your data into a model that speaks your language.');s.badge(1272,98,'LoRA training',136)
    s.card(272,180,552,718,'Training configuration');s.text(296,246,'MODEL & DATA',10,'ice',500)
    s.field(296,275,504,'Base model','meta-llama / Llama-3.2-1B-Instruct',True)
    s.field(296,367,352,'Dataset','support-conversations.jsonl',True);s.button(664,379,136,'Import file',icon='file-up')
    s.field(296,459,504,'Adapter name','support-assistant-v2')
    s.line(296,532,504);s.text(296,561,'HYPERPARAMETERS',10,'ice',500)
    for x,y,w,l,v in [(296,590,240,'LoRA rank','8'),(560,590,240,'LoRA alpha','16'),(296,677,152,'Epochs','3'),(472,677,152,'Learning rate','0.0002'),(648,677,152,'Batch size','1')]:s.field(x,y,w,l,v)
    s.text(296,774,'Advanced parameters',14,'text');s.text(784,774,'⌄',16,'ice')
    s.toggle(296,800);s.text(346,815,'QLoRA · 4-bit quantization',14)
    s.toggle(296,844);s.text(346,859,'Cache adapter for hot-switching',14)
    s.card(848,180,560,286,'Training run');s.badge(1264,202,'Running',120)
    s.text(872,256,'support-assistant-v2',14,'text');s.heading(872,315,'64%',40);s.text(995,309,'Step 384 of 600',14,'text')
    s.progress(872,340,512,.64);s.text(872,377,'Epoch 2 / 3',12,'text');s.text(1384,377,'About 4 min remaining',12,'text',anchor='end')
    s.button(872,400,168,'Start training',False,icon='play',disabled=True);s.button(1056,400,136,'Stop run',False,icon='circle-stop')
    s.card(848,490,560,188,'Live metrics')
    for x,label,val in [(872,'TRAINING LOSS','0.842'),(1050,'LEARNING RATE','0.0002'),(1230,'ELAPSED','07:42')]:s.text(x,564,label,10,'text');s.heading(x,606,val,26)
    s.text(872,650,'Loss is reported by the current training job.',12,'text')
    s.card(848,702,560,196,'Training log');s.badge(1300,722,'Live',84)
    for j,t in enumerate(['[07:31]  Step 380   loss=0.867','[07:36]  Step 382   loss=0.851','[07:42]  Step 384   loss=0.842','[07:42]  Checkpoint saved']):s.text(872,774+j*27,t,13,'ice' if j==3 else 'text',font='monospace')
    return s

def rag():
    s=SVG('RAG');s.shell('Give your model the context it needs.');s.button(1220,90,188,'Add documents',True,'plus')
    s.card(272,180,712,430,'Your documents');s.text(960,220,'3 files · 186 chunks',12,'text',anchor='end')
    files=[('product-handbook.pdf','PDF','96 chunks · 2.4 MB'),('support-playbook.md','MD','62 chunks · 184 KB'),('release-notes.txt','TXT','28 chunks · 42 KB')]
    for i,(n,kind,meta) in enumerate(files):
        y=248+i*94;s.rect(296,y,664,78,'deep',8,'border');s.icon(312,y+26,'file-text','ice',24);s.text(352,y+31,n,14,weight=500);s.text(352,y+55,meta,12,'text');s.badge(826,y+14,kind,60);s.icon(916,y+29,'trash','text',18)
    s.icon(296,562,'check','ice',18);s.text(325,576,'All documents indexed and ready for retrieval.',12,'text')
    s.card(1008,180,400,430,'Retrieval configuration')
    s.field(1032,269,352,'Chunk size (tokens)','512');s.field(1032,357,352,'Chunk overlap (tokens)','64');s.field(1032,445,352,'Top-K retrieved chunks','4')
    s.text(1032,557,'Retrieve the most relevant passages',12,'text');s.text(1032,577,'and attach them to your conversation.',12,'text')
    s.card(272,634,1136,264,'Test retrieval')
    s.rect(296,697,896,44,'deep',8,'border');s.icon(312,710,'search','text',18);s.text(346,724,'How do I configure a local inference engine?');s.button(1208,698,176,'Search',True)
    s.line(296,765,1088);s.badge(296,786,'01',44);s.text(356,804,'product-handbook.pdf',14,weight=500);s.text(1384,804,'Similarity 0.92',12,'ice',anchor='end')
    s.text(356,835,'Choose Auto in Settings to detect your hardware, or select a local engine explicitly.',14,'text');s.text(356,862,'Chunk 18 · Engine configuration',12,'text')
    return s

def adapters():
    s=SVG('Adapters');s.shell('Specialized skills. One base model.');s.button(1220,90,188,'Import adapter',True,'file-up')
    s.group('Active-adapter');s.card(272,180,1136,212);s.rect(272,180,4,212,'ice',2)
    s.text(300,216,'ACTIVE ADAPTER',10,'ice',500);s.heading(300,253,'support-assistant-v2',24);s.text(300,283,'Base model: meta-llama / Llama-3.2-1B-Instruct',14,'text')
    s.badge(300,309,'LoRA',68);s.badge(380,309,'Rank 8',84);s.text(484,327,'12.8 MB  ·  Trained 21 Sep 2026',12,'text')
    s.button(1128,230,256,'✓  Active in this session');s.text(1384,306,'Ready to use in Chat',12,'ice',anchor='end');s.end()
    s.heading(272,442,'Adapter library');s.text(1408,442,'3 adapters',12,'text',anchor='end')
    for i,(title,base,rank,sz) in enumerate([('writing-companion','Llama-3.2-1B-Instruct','16','25.6 MB'),('code-review-assistant','Qwen2.5-3B-Instruct','8','18.4 MB')]):
        x=272+i*580;s.card(x,470,556,266);s.icon(x+24,494,'layers','ice',24);s.badge(x+376,494,'Available',156);s.heading(x+24,554,title);s.text(x+24,586,'Base: '+base,13,'text');s.text(x+24,617,f'LoRA   ·   Rank {rank}   ·   {sz}',12,'text');s.line(x+24,640,508)
        s.button(x+24,666,156,'Activate',True);s.button(x+192,666,132,'Export',False,'file-down');s.button(x+336,666,196,'Delete adapter',False,'trash')
    s.card(272,760,1136,138,'Switch with confidence')
    s.text(296,828,'An adapter works with its matching base model. Load that model before activating its adapter.',14,'text')
    s.text(296,858,'For llama.cpp, GGUF adapters are applied when the model loads.',12,'ice')
    return s

def chat():
    s=SVG('Chat');s.shell('A conversation with your own intelligence.');s.button(1264,90,144,'Clear chat')
    s.card(272,180,1136,90);s.icon(296,210,'cpu','ice',24);s.heading(338,218,'Llama 3.2 1B Instruct',18)
    s.text(338,247,'Local engine · llama.cpp',12,'text');s.badge(790,210,'Adapter: none',136);s.badge(950,210,'✓ Model loaded',160);s.button(1184,204,200,'Refresh status',False,'refresh-cw')
    s.card(272,294,768,464)
    s.text(656,332,'YOU',10,'text',500);s.rect(432,349,584,84,'raised',12);s.text(452,381,'Explain how retrieval-augmented generation works.');s.text(452,407,'Keep it practical and concise.');s.text(296,475,'MAGNITE',10,'ice',500)
    s.rect(296,491,660,218,'deep',12);s.rect(296,505,2,186,'ice',1)
    lines=['RAG gives a language model relevant information from','your documents before it answers.','1. Split your documents into searchable chunks.','2. Retrieve the chunks closest to your question.','3. Include those passages in the model’s context.','You get answers grounded in your own material.']
    for i,t in enumerate(lines):s.text(320,523+i*31,t,14,'white' if i<2 else 'text')
    s.text(320,739,'Response complete',12,'text');s.icon(978,724,'check','ice',18)
    s.card(1064,294,344,464,'Generation settings')
    s.field(1088,378,296,'Model','Llama 3.2 1B Instruct',True);s.field(1088,466,296,'Engine','Auto (detect hardware)',True)
    s.field(1088,554,136,'Max tokens','256');s.field(1248,554,136,'Temperature','0.70');s.button(1088,638,296,'Load model',True)
    s.text(1088,720,'Runs on your machine.',12,'text')
    s.card(272,782,1136,116);s.text(296,818,'Type a message…',14,'text');s.toggle(296,850,False);s.text(345,865,'Use RAG',12,'text');s.text(472,865,'Enter to send',12,'muted');s.button(1240,819,144,'Send',True,'send')
    return s

def settings():
    s=SVG('Settings');s.shell('Make this workspace yours.');s.button(1232,90,176,'Save settings',True)
    s.card(272,180,552,258,'Engine & storage')
    s.field(296,267,504,'Inference engine','Auto (detect hardware)',True);s.field(296,358,360,'Data directory','~/.magnite');s.button(672,370,128,'Browse',False,'folder')
    s.card(848,180,560,258,'Appearance');s.badge(1272,200,'Dark only',112)
    s.rect(872,247,98,69,'bg',8,'border');s.rect(881,256,22,51,'raised',3);s.rect(910,256,50,18,'blue',3);s.rect(910,281,50,26,'card',3)
    s.heading(992,271,'Midnight',16);s.text(992,299,'Deep blue. White stars.',12,'text');s.line(872,337,512)
    s.text(872,367,'Floating stars',14);s.toggle(1348,351);s.text(872,408,'Reduce motion',14);s.toggle(1348,392,False)
    s.card(272,462,552,330,'Hosted inference');s.badge(704,482,'Optional',96)
    s.field(296,549,504,'OpenAI-compatible endpoint','https://api.example.com/v1');s.field(296,640,504,'API key','Not configured')
    s.text(296,737,'Local inference does not require an API key.',12,'text');s.text(296,762,'Hosted requests use your configured provider.',12,'text')
    s.card(848,462,560,330,'Privacy');s.icon(872,530,'cpu','ice',24);s.heading(912,550,'Local by default',18)
    s.text(872,591,'Models, adapters, and documents stay in your',14,'text');s.text(872,615,'local data directory.',14,'text');s.line(872,644,512)
    s.text(872,681,'Anonymous usage data',14);s.toggle(1348,665,False);s.text(872,713,'Off · sharing is opt-in.',12,'ice');s.text(872,761,'You stay in control of your workspace.',12,'text')
    s.card(272,816,1136,82);s.icon(296,846,'check','ice');s.text(332,851,'One focused theme',14,weight=500);s.text(332,876,'Additional color themes will be explored later.',12,'text')
    return s

def main():
    (HERE/'panels').mkdir(exist_ok=True)
    paths=[fn().save(i) for i,fn in enumerate([models,training,rag,adapters,chat,settings],1)]
    (HERE/'tokens.json').write_text(json.dumps(C,indent=2)+'\n')
    fonts=HERE/'fonts';fonts.mkdir(exist_ok=True)
    for p in (ROOT/'app/resources/fonts').iterdir():
        if p.suffix in ['.ttf','.txt']:shutil.copy2(p,fonts/p.name)
    styles='''@font-face{font-family:"DM Sans";src:url(fonts/DMSans-Regular.ttf)}@font-face{font-family:"DM Sans";src:url(fonts/DMSans-Medium.ttf);font-weight:500}@font-face{font-family:"Space Grotesk";src:url(fonts/SpaceGrotesk-SemiBold.ttf);font-weight:600}*{box-sizing:border-box}body{margin:0;background:#030914;color:white;font:14px "DM Sans",sans-serif}header{display:flex;gap:20px;align-items:center;padding:18px 24px;border-bottom:1px solid #243D5E;flex-wrap:wrap}header strong{margin-right:auto}nav{display:flex;gap:8px;flex-wrap:wrap}button{font:inherit;color:#B3C7E1;border:1px solid #243D5E;background:#0C1B30;border-radius:8px;padding:10px 16px;cursor:pointer}button[aria-pressed=true]{background:#2563EB;color:white}button:focus-visible,input:focus-visible{outline:2px solid #93C5FD;outline-offset:3px}main{max-width:1500px;margin:24px auto;padding:0 24px}section[hidden]{display:none}svg{display:block;width:100%;height:auto;border:1px solid #243D5E;border-radius:12px}footer{color:#B3C7E1;padding:0 24px 24px;text-align:center}body.motion g[id=Starfield]{animation:drift 24s ease-in-out infinite alternate}@keyframes drift{to{transform:translate(8px,-12px)}}@media(prefers-reduced-motion:reduce){body.motion g[id=Starfield]{animation:none}}'''
    html=['<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Magnite · Midnight panels</title><style>'+styles+'</style><header><strong>MAGNITE / MIDNIGHT</strong><nav aria-label="Panel previews">']
    for i,n in enumerate(NAMES):html.append(f'<button type="button" data-index="{i}" aria-pressed="{str(i==0).lower()}">{n}</button>')
    html.append('</nav><label><input type="checkbox" id="motion"> Preview star drift</label></header><main>')
    for i,p in enumerate(paths):
        import re
        svg=re.sub(r'id="([^"]+)"',lambda m:f'id="panel-{i}-{m[1]}"'+(' class="starfield"' if m[1]=='Starfield' else ''),p.read_text())
        html.append(f'<section aria-label="{NAMES[i]} preview"'+(' hidden' if i else '')+'>'+svg+'</section>')
    html.append('''</main><footer>Design previews with illustrative data. Screen controls are static. Import the six SVG files from panels/ into Figma.</footer><script>const buttons=[...document.querySelectorAll('nav button')],panels=[...document.querySelectorAll('main section')];buttons.forEach((b,i)=>b.addEventListener('click',()=>{buttons.forEach((x,j)=>x.setAttribute('aria-pressed',i===j));panels.forEach((x,j)=>x.hidden=i!==j)}));document.querySelector('#motion').addEventListener('change',e=>document.body.classList.toggle('motion',e.target.checked));</script></html>''')
    (HERE/'preview.html').write_text('\n'.join(html).replace('g[id=Starfield]', '.starfield'))
    print('\n'.join(str(p.relative_to(ROOT)) for p in paths))

if __name__=='__main__':main()
