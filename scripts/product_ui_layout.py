"""Preview-only: no firmware files or serial operations."""
from pathlib import Path
import ast
import os
from PIL import Image,ImageDraw,ImageFont
ROOT=Path(__file__).resolve().parents[1]
PREVIEW_ROOT=ROOT/'tmp/ui-previews'
SOURCE=ROOT/'scripts/generate_diagnostic_pages.py'
# Load only the icon function, never the generator's top-level file writes.
tree=ast.parse(SOURCE.read_text(encoding='utf-8'))
node=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='icon')
ns={'Image':Image,'ImageDraw':ImageDraw}
exec(compile(ast.Module(body=[node],type_ignores=[]),str(SOURCE),'exec'),ns)
base_icon=ns['icon']
FONT_ROOT=ROOT/'assets/fonts/fusion-pixel'
# Render pixel fonts at their native sizes, without rescaling or antialiasing.
# The system option is for the before/after preview only.
SYSTEM_FONT=os.environ.get('ESTOP_UI_FONT')=='system'
CN=ImageFont.truetype('C:/Windows/Fonts/simsun.ttc' if SYSTEM_FONT else str(FONT_ROOT/'fusion-pixel-12px-monospaced-zh_hans.otf'),12)
FOOT=ImageFont.truetype('C:/Windows/Fonts/simsun.ttc' if SYSTEM_FONT else str(FONT_ROOT/'fusion-pixel-10px-monospaced-zh_hans.otf'),10)
SOC=ImageFont.truetype('C:/Windows/Fonts/arial.ttf',12)
BIG=ImageFont.truetype('C:/Windows/Fonts/consola.ttf',24)
HEAD=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',18)

def text(d,x,y,s,font=CN,fill=1,limit=None):
    b=d.textbbox((0,0),s,font=font);w=b[2]-b[0];h=b[3]-b[1]
    assert 0<=x and x+w<=128 and 0<=y and y+h<=64,(s,x,y,w,h)
    if limit is not None:assert w<=limit,(s,w,limit)
    d.text((x-b[0],y-b[1]),s,font=font,fill=fill)

def center(d,s,y,font=CN,left=0,width=71,fill=1):
    b=d.textbbox((0,0),s,font=font)
    text(d,left+(width-(b[2]-b[0]))//2,y,s,font,fill,limit=width)

def symbol(name):
    im=Image.new('1',(48,36));d=ImageDraw.Draw(im)
    if name in ['ready','stopping','starting','short_check','short_fault','protected','offline','unpaired']:
        return base_icon(name)
    if name in ['on','off']:
        active=name=='on';d.rounded_rectangle((3,9,43,26),radius=8,outline=1,fill=int(active));x=27 if active else 6
        d.ellipse((x,12,x+12,23),fill=int(not active))
    elif name=='moon':
        d.ellipse((8,0,39,31),fill=1);d.ellipse((19,-4,44,22),fill=0)
    elif name=='sun':
        d.ellipse((16,10,32,26),outline=1,width=2)
        for line in [(24,1,24,6),(24,30,24,35),(7,18,12,18),(36,18,41,18),(11,5,15,9),(34,28,38,32),(34,8,38,4),(11,31,15,27)]:d.line(line,fill=1,width=2)
    elif name=='temperature':
        d.rounded_rectangle((18,1,28,26),radius=5,outline=1,width=2);d.ellipse((15,22,31,35),outline=1,width=2)
        d.line((23,10,23,30),fill=1,width=3)
        for y in [8,14,20]:d.line((31,y,36,y),fill=1,width=2)
    elif name in ['overvoltage','undervoltage']:
        d.line([(4,5),(14,31),(24,5)],fill=1,width=3)
        top,bottom=(4,30) if name=='overvoltage' else (30,4)
        d.line((37,top,37,bottom),fill=1,width=3)
        dy=7 if name=='overvoltage' else -7
        d.line([(30,top+dy),(37,top),(44,top+dy)],fill=1,width=3)
    elif name=='overcurrent':
        d.polygon([(27,0),(13,19),(24,19),(19,35),(39,13),(27,13)],outline=1,width=2)
        d.line((5,10,5,23),fill=1,width=3);d.rectangle((4,29,6,31),fill=1)
    elif name=='battery':
        d.rounded_rectangle((3,8,40,29),radius=2,outline=1,width=2);d.rectangle((41,14,45,23),fill=1)
        d.line((22,11,22,20),fill=1,width=2);d.rectangle((21,23,23,25),fill=1)
    elif name=='usb':
        d.line((17,0,17,9),fill=1,width=3);d.line((30,0,30,9),fill=1,width=3)
        d.rounded_rectangle((11,8,36,25),radius=3,outline=1,width=2);d.line((24,25,24,35),fill=1,width=3)
    elif name=='return':
        d.line([(38,28),(38,13),(10,13)],fill=1,width=3);d.line([(20,3),(10,13),(20,23)],fill=1,width=3)
    elif name=='info':
        d.ellipse((8,0,40,32),outline=1,width=2);d.rectangle((23,7,26,10),fill=1);d.rectangle((23,15,26,25),fill=1)
    elif name=='check':
        d.line([(8,17),(20,29),(40,6)],fill=1,width=3)
    else:raise ValueError(name)
    return im

def rail(im,state='关闭',soc=30,charging=False):
    d=ImageDraw.Draw(im);d.line((71,2,71,61),fill=1)
    if state in ['开启','关闭']:
        on=state=='开启';d.rounded_rectangle((81,4,121,21),radius=8,outline=1,fill=int(on));x=105 if on else 84
        d.ellipse((x,7,x+12,18),fill=int(not on))
    elif state=='未连接':
        d.rounded_rectangle((80,7,97,18),radius=5,outline=1,width=2);d.rounded_rectangle((105,7,122,18),radius=5,outline=1,width=2)
        d.line((99,5,103,21),fill=1,width=2)
    else:
        # Pending state is an hourglass; it cannot resemble an OFF confirmation.
        d.line((90,4,111,4),fill=1,width=2);d.line((90,21,111,21),fill=1,width=2)
        d.line([(92,5),(92,8),(108,18),(108,20)],fill=1,width=2);d.line([(109,5),(109,8),(93,18),(93,20)],fill=1,width=2)
    center(d,state,26,CN,72,56)
    d.rounded_rectangle((80,44,120,61),radius=2,outline=1);d.rectangle((121,49,123,56),fill=1)
    if charging:d.polygon([(77,47),(73,53),(76,53),(74,58),(79,51),(76,51)],fill=1)
    center(d,f'{soc}%',49,SOC,81,39)

def state_page(name,title,footer,state='关闭',soc=30):
    im=Image.new('1',(128,64));im.paste(symbol(name),(11,1));d=ImageDraw.Draw(im)
    center(d,title,40);center(d,footer,54,FOOT);rail(im,state,soc);return im

def failed(critical=False,usb=False):
    if critical:return state_page('stopping','关闭未确认','仍在重试','待确认')
    return state_page('offline','连接失败','短按重试','未连接')

MENU=['返回主页','常亮已关','手动休眠','休眠时间','开始配对','重新配对','设备信息']
def menu(selected):
    im=Image.new('1',(128,64));d=ImageDraw.Draw(im)
    center(d,MENU[(selected-1)%7],4)
    d.rounded_rectangle((1,20,69,37),radius=2,fill=1)
    center(d,MENU[selected],23,fill=0)
    center(d,MENU[(selected+1)%7],40)
    center(d,'短选 长确认',54,FOOT)
    rail(im);return im

def confirm(title,name,selected=False):
    im=Image.new('1',(128,64));d=ImageDraw.Draw(im);center(d,title,2)
    im.paste(symbol(name).resize((32,24),Image.Resampling.NEAREST),(20,17))
    for x,label,active in [(1,'取消',not selected),(36,'确认',selected)]:
        d.rounded_rectangle((x,46,x+32,62),radius=2,outline=1,fill=int(active))
        text(d,x+4,48,label,fill=int(not active))
    rail(im);return im

def info():
    im=Image.new('1',(128,64));d=ImageDraw.Draw(im)
    center(d,'电池电压',3);center(d,'3.57 V',21,ImageFont.truetype('C:/Windows/Fonts/consola.ttf',17))
    center(d,'按键返回',52,FOOT);rail(im);return im

GROUPS={
 'actions':[
 ('正在关闭，尚未确认',state_page('stopping','正在关闭','等待确认','待确认')),
 ('关闭已确认',state_page('check','已关闭','急停已生效')),
 ('正在开启，不能提前显示开',state_page('starting','正在开启','等待检测','待确认')),
 ('短路检测，仅有明确状态时显示',state_page('short_check','短路检测','暂未开启')),
 ('急停关闭通信失败，保留重试',failed(True)),
 ('开启确认，随后回数据页',state_page('check','已开启','返回数据','开启'))],
 'protection':[
 ('短路：检查负载',state_page('short_fault','检测到短路','检查负载')),
 ('过温：等待降温',state_page('temperature','温度保护','等待降温')),
 ('过压：检查输入电压',state_page('overvoltage','过压保护','检查电源')),
 ('欠压：检查输入电压',state_page('undervoltage','欠压保护','检查电源')),
 ('过流：检查负载',state_page('overcurrent','过流保护','检查负载')),
 ('检测器异常：不伪装成短路',state_page('protected','检测异常','检查设备'))],
 'connection':[
 ('连接中',state_page('unpaired','正在连接','请稍候','待确认')),
 ('未配对：长按进入菜单',state_page('unpaired','尚未配对','长按菜单','未连接')),
 ('失败等待：电池供电',failed()),
 ('失败等待：插电保持显示',failed(usb=True)),
 ('正在配对：需要对端参与',state_page('unpaired','正在配对','对端配对','未连接')),
 ('配对成功：仍须确认关闭',state_page('check','配对成功','同步关闭','待确认'))],
 'menus':[
 ('菜单：当前选中常亮模式',menu(1)),
 ('菜单：当前选中手动休眠',menu(2)),
 ('开启常亮：默认取消',confirm('开启常亮','sun')),
 ('休眠确认：选中确认',confirm('进入休眠','moon',True)),
 ('重新配对：第二次确认',confirm('删除配对','unpaired')),
 ('设备信息：电压与 SOC',info())],
 'power':[
 ('插电：休眠被阻止',state_page('usb','无法休眠','请拔电源')),
 ('输出开启：休眠被阻止',state_page('on','无法休眠','请先关闭','开启')),
 ('状态未确认：休眠被阻止',state_page('stopping','无法休眠','等待确认','待确认')),
 ('确认关闭后准备睡眠',state_page('moon','准备休眠','按键可唤醒')),
 ('低电量：不绕过输出安全判断',state_page('battery','电量偏低','请接电源',soc=8)),
 ('常亮开启：已保存',state_page('sun','常亮已开','设置已保存'))],
 'other':[
 ('旧设备未提供原因',state_page('protected','开启拒绝','检查对端')),
 ('冷却等待：不自动开启',state_page('starting','冷却等待','稍后再试')),
 ('设备忙碌',state_page('starting','设备忙碌','稍后再试')),
 ('尚未就绪',state_page('protected','尚未就绪','检查对端')),
 ('保护触发但无具体类型',state_page('protected','保护触发','检查对端')),
 ('保存失败：不能显示已保存',state_page('protected','保存失败','重新操作'))],
 'remaining-menus':[(f'菜单：{label}',menu(i)) for i,label in enumerate(MENU)]
}

def sheet(items,path,columns=2,scale=4):
    w=128*scale;h=64*scale;cellw=w+36;cellh=h+60;rows=(len(items)+columns-1)//columns
    im=Image.new('RGB',(columns*cellw+12,rows*cellh+12),'#171b21');d=ImageDraw.Draw(im)
    for i,(label,native) in enumerate(items):
        x=18+(i%columns)*cellw;y=12+(i//columns)*cellh
        d.text((x,y),label,font=HEAD,fill='#dce6f0')
        im.paste(native.convert('RGB').resize((w,h),Image.Resampling.NEAREST),(x,y+32))
    im.save(path)

if __name__=='__main__':
    PREVIEW_ROOT.mkdir(parents=True,exist_ok=True)
    for name,items in GROUPS.items():
        for i,(label,im) in enumerate(items):im.save(PREVIEW_ROOT/f'other-{name}-{i+1}-native.png')
        sheet(items,PREVIEW_ROOT/f'other-{name}-preview.png')
    key=[GROUPS['actions'][0],GROUPS['protection'][0],GROUPS['connection'][2],GROUPS['menus'][0],GROUPS['menus'][2],GROUPS['power'][0]]
    sheet(key,PREVIEW_ROOT/'other-ui-overview.png')
    print('Generated preview-only 128x64 screens with text boundary assertions; no firmware assets changed.')
