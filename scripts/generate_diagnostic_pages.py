"""生成诊断页面位图与同源预览；需要 Pillow 和 Windows 宋体字库。"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONT = ImageFont.truetype('C:/Windows/Fonts/simsun.ttc', 16)
SMALL = ImageFont.truetype('C:/Windows/Fonts/simsun.ttc', 12)
DIGITS = ImageFont.truetype('C:/Windows/Fonts/consolab.ttf', 24)
PAGES = [
    ('ready', '屏幕就绪', '急停开关', '等待后续联调'),
    ('off', '输出关闭', '急停按下', '对端确认关闭'),
    ('on', '输出开启', '急停释放', '对端确认开启'),
    ('stopping', '正在关闭', '等待确认', '关闭请求处理中'),
    ('starting', '正在开启', '等待确认', '开启请求处理中'),
    ('short_check', '短路检测', '暂未开启', '正在检查负载'),
    ('short_fault', '禁止开启', '检测到短路', '请检查负载'),
    ('protected', '保护触发', '过流保护', '输出保持关闭'),
    ('offline', '通信中断', '关闭未确认', '正在重试关闭'),
    ('unpaired', '尚未配对', '无法控制', '请先完成配对'),
]

def icon(name):
    """原生单色像素图标，主轮廓用 2–3 像素线条。"""
    im = Image.new('1', (48, 36)); d = ImageDraw.Draw(im)
    octagon = [(14,1),(33,1),(44,11),(44,24),(33,34),(14,34),(3,24),(3,11)]
    if name == 'ready':
        d.rounded_rectangle((2,1,45,29), radius=3, outline=1, width=2)
        d.line((14,34,34,34), fill=1, width=2)
        d.line((24,29,24,34), fill=1, width=2)
        d.line((12,15,20,22,35,7), fill=1, width=3)
    elif name == 'off':
        d.rounded_rectangle((0,5,47,30), radius=12, outline=1, width=2)
        d.ellipse((4,9,21,26), fill=1)
        d.ellipse((30,13,38,22), outline=1, width=2)
    elif name == 'on':
        d.rounded_rectangle((0,5,47,30), radius=12, fill=1)
        d.ellipse((26,9,43,26), fill=0)
        d.line((13,12,13,23), fill=0, width=3)
    elif name == 'stopping':
        d.polygon(octagon, outline=1, width=2)
        d.rectangle((17,9,30,21), fill=1)
        for x in (17,23,29): d.rectangle((x,26,x+2,28), fill=1)
    elif name == 'starting':
        d.line((8,2,31,2), fill=1, width=3)
        d.line((8,33,31,33), fill=1, width=3)
        d.line((11,3,11,9,27,26,27,32), fill=1, width=2)
        d.line((28,3,28,9,12,26,12,32), fill=1, width=2)
        d.polygon([(14,7),(25,7),(20,14)], fill=1)
        d.polygon([(20,24),(14,30),(26,30)], fill=1)
        d.polygon([(37,11),(45,18),(37,25)], fill=1)
    elif name == 'short_check':
        d.ellipse((2,0,32,29), outline=1, width=3)
        d.line((29,25,40,35), fill=1, width=5)
        d.polygon([(19,5),(10,16),(17,16),(14,25),(25,12),(19,12)], fill=1)
    elif name == 'short_fault':
        d.polygon([(24,0),(47,34),(0,34)], outline=1, width=3)
        d.polygon([(25,8),(17,21),(23,21),(21,29),(32,17),(25,17)], fill=1)
    elif name == 'protected':
        d.polygon([(24,0),(43,6),(40,22),(34,29),(24,35),(14,29),(8,22),(5,6)], outline=1, width=3)
        d.rectangle((22,9,26,21), fill=1)
        d.rectangle((22,25,26,28), fill=1)
    elif name == 'offline':
        d.arc((0,0,46,40), 218, 322, fill=1, width=3)
        d.arc((8,9,38,38), 218, 322, fill=1, width=3)
        d.ellipse((21,27,26,32), fill=1)
        d.line((34,20,45,31), fill=1, width=3)
        d.line((45,20,34,31), fill=1, width=3)
    elif name == 'unpaired':
        d.rounded_rectangle((0,11,20,26), radius=7, outline=1, width=3)
        d.rounded_rectangle((27,11,47,26), radius=7, outline=1, width=3)
        d.rectangle((17,10,30,27), fill=0)
        for line in [(21,1,18,7),(27,1,30,7),(21,35,18,29),(27,35,30,29)]:
            d.line(line, fill=1, width=2)
    return im

def centered(draw, y, text, font):
    box = draw.textbbox((0, 0), text, font=font)
    width = box[2] - box[0]
    assert width <= 128, text
    draw.text(((128-width)//2-box[0], y-box[1]), text, font=font, fill=1)

def telemetry(online=True):
    """电压/电流常驻主页的模拟快照；失联时不保留看似实时的数值。"""
    im = Image.new('1', (128,64)); d = ImageDraw.Draw(im)
    d.text((1,-1), 'DEMO', font=SMALL, fill=1)
    if online:
        # 信号连接标志和输出开启标志；此处均为模拟值。
        for x, top in [(77,7),(81,4),(85,1)]:
            d.rectangle((x,top,x+1,10), fill=1)
        d.rounded_rectangle((99,1,124,10), radius=4, fill=1)
        d.ellipse((116,3,121,8), fill=0)
        d.line((106,3,106,8), fill=0, width=2)
    else:
        d.text((75,-1), '数据离线', font=SMALL, fill=1)
    d.line((0,12,127,12), fill=1)
    for y, number, unit in [(15,'12.34' if online else '--.--','V'),
                            (35,'2.400' if online else '-.---','A')]:
        box=d.textbbox((0,0),number,font=DIGITS)
        d.text((96-box[2],y-box[1]),number,font=DIGITS,fill=1)
        box=d.textbbox((0,0),unit,font=FONT)
        d.text((108-box[0],y+3-box[1]),unit,font=FONT,fill=1)
    d.line((0,54,127,54),fill=1)
    # 页底的功率为电压乘电流，当前仅用于布局验证。
    text='P  29.62 W' if online else 'P  --.-- W'
    box=d.textbbox((0,0),text,font=SMALL)
    d.text((2,55-box[1]),text,font=SMALL,fill=1)
    d.text((100,55-box[1]),'USB',font=SMALL,fill=1)
    return im

def build():
    images = []
    for idx, (name, title, detail, footer) in enumerate(PAGES):
        im = Image.new('1', (128, 64))
        d = ImageDraw.Draw(im)
        d.text((2, -1), 'DEMO', font=SMALL, fill=1)
        d.text((66, -1), f'USB {idx:02d}/09', font=SMALL, fill=1)
        d.line((0, 12, 127, 12), fill=1)
        im.paste(icon(name), (3,15))
        for y, text, font in [(18,title,FONT),(38,detail,SMALL)]:
            box = d.textbbox((0,0), text, font=font)
            assert box[2]-box[0] <= 68, text
            d.text((58-box[0],y-box[1]),text,font=font,fill=1)
        centered(d, 52, footer, SMALL)
        images.append(im)
    images.extend([telemetry(True), telemetry(False)])
    names = [p[0] for p in PAGES] + ['meter', 'meter_offline', 'pixel_test']
    test = Image.new('1', (128, 64)); d = ImageDraw.Draw(test)
    d.rectangle((0, 0, 127, 63), outline=1)
    d.line((0, 0, 127, 63), fill=1); d.line((127, 0, 0, 63), fill=1)
    d.rectangle((28, 21, 99, 43), fill=0, outline=1)
    centered(d, 26, '128x64', SMALL)
    images.append(test)
    header = ['// Generated by scripts/generate_diagnostic_pages.py; demo pixels only.', '#pragma once', '#include <stdint.h>', 'static const uint8_t kPages[][1024] = {']
    for im in images:
        data = [sum((1 << bit) if im.getpixel((x, page*8+bit)) else 0 for bit in range(8)) for page in range(8) for x in range(128)]
        header.append('{' + ','.join(f'0x{n:02x}' for n in data) + '},')
    header.append('};')
    header.append('static const char* const kPageNames[] = {' + ','.join('"'+name+'"' for name in names) + '};')
    header.append(f'static constexpr int kPageCount = {len(images)};')
    (ROOT/'components/app/emergency_ui/pages.h').write_text('\n'.join(header)+'\n', encoding='utf-8')
    out = ROOT/'tmp/ui-previews'; out.mkdir(parents=True, exist_ok=True)
    sheet = Image.new('RGB', (3*404, ((len(images)+2)//3)*230), '#202329')
    for n, im in enumerate(images):
        x, y = (n%3)*404+10, (n//3)*230+24
        sheet.paste(im.convert('RGB').resize((384,192), Image.Resampling.NEAREST), (x,y))
        ImageDraw.Draw(sheet).text((x,y-18), str(n)+' '+names[n], fill='white')
    sheet.save(out/'diagnostic-preview.png')
    images[10].resize((512,256),Image.Resampling.NEAREST).save(out/'meter-preview.png')

if __name__ == '__main__': build()
