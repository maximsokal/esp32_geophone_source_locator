#include "web_seismic_page.h"


static const char s_html[] =
    "<!DOCTYPE html>"
    "<html lang=\"en\">"
    "<head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" "
        "content=\"width=device-width,initial-scale=1\">"

        "<title>GeoLocator Seismic</title>"

        "<style>"
            "*{box-sizing:border-box;}"

            "body{"
                "margin:0;"
                "background:#0d1117;"
                "color:#e6edf3;"
                "font-family:Arial,sans-serif;"
            "}"

            ".page{"
                "max-width:1100px;"
                "margin:0 auto;"
                "padding:18px;"
            "}"

            "a{"
                "color:#58a6ff;"
                "text-decoration:none;"
            "}"

            ".header{"
                "display:flex;"
                "justify-content:space-between;"
                "align-items:center;"
                "gap:16px;"
                "margin:12px 0 20px;"
            "}"

            ".title{"
                "font-size:28px;"
                "font-weight:700;"
            "}"

            ".status{"
                "font-size:12px;"
                "font-weight:700;"
            "}"

            ".ok{color:#3fb950;}"
            ".warning{color:#d29922;}"
            ".error{color:#f85149;}"
            ".muted{color:#8b949e;}"

            ".meta{"
                "display:grid;"
                "grid-template-columns:"
                    "repeat(4,minmax(0,1fr));"
                "gap:10px;"
                "margin-bottom:16px;"
            "}"

            ".meta-item,"
            ".trace{"
                "background:#161b22;"
                "border:1px solid #30363d;"
                "border-radius:10px;"
            "}"

            ".meta-item{"
                "padding:12px;"
            "}"

            ".meta-label{"
                "font-size:11px;"
                "color:#8b949e;"
                "margin-bottom:5px;"
            "}"

            ".meta-value{"
                "font-weight:700;"
            "}"

            ".trace{"
                "padding:12px;"
                "margin-bottom:12px;"
            "}"

            ".trace-head{"
                "display:flex;"
                "justify-content:space-between;"
                "margin-bottom:8px;"
                "font-size:13px;"
            "}"

            ".trace-name{"
                "font-weight:700;"
            "}"

            "canvas{"
                "display:block;"
                "width:100%;"
                "height:120px;"
                "background:#0d1117;"
                "border-radius:6px;"
            "}"

            "@media(max-width:700px){"
                ".meta{"
                    "grid-template-columns:"
                        "repeat(2,minmax(0,1fr));"
                "}"
            "}"
        "</style>"
    "</head>"

    "<body>"
        "<main class=\"page\">"

            "<a href=\"/\">&larr; Dashboard</a>"

            "<div class=\"header\">"
                "<div class=\"title\">Seismic Monitor</div>"
                "<div "
                    "id=\"status\" "
                    "class=\"status muted\""
                ">"
                    "WAITING"
                "</div>"
            "</div>"

            "<div class=\"meta\">"

                "<div class=\"meta-item\">"
                    "<div class=\"meta-label\">Source</div>"
                    "<div id=\"source\" class=\"meta-value\">--</div>"
                "</div>"

                "<div class=\"meta-item\">"
                    "<div class=\"meta-label\">Sample rate</div>"
                    "<div id=\"rate\" class=\"meta-value\">--</div>"
                "</div>"

                "<div class=\"meta-item\">"
                    "<div class=\"meta-label\">Window</div>"
                    "<div id=\"window\" class=\"meta-value\">--</div>"
                "</div>"

                "<div class=\"meta-item\">"
                    "<div class=\"meta-label\">Frames</div>"
                    "<div id=\"frames\" class=\"meta-value\">--</div>"
                "</div>"

            "</div>"

            "<div class=\"trace\">"
                "<div class=\"trace-head\">"
                    "<span class=\"trace-name\">Geophone 1</span>"
                    "<span id=\"range1\" class=\"muted\">--</span>"
                "</div>"
                "<canvas id=\"graph1\"></canvas>"
            "</div>"

            "<div class=\"trace\">"
                "<div class=\"trace-head\">"
                    "<span class=\"trace-name\">Geophone 2</span>"
                    "<span id=\"range2\" class=\"muted\">--</span>"
                "</div>"
                "<canvas id=\"graph2\"></canvas>"
            "</div>"

            "<div class=\"trace\">"
                "<div class=\"trace-head\">"
                    "<span class=\"trace-name\">Geophone 3</span>"
                    "<span id=\"range3\" class=\"muted\">--</span>"
                "</div>"
                "<canvas id=\"graph3\"></canvas>"
            "</div>"

            "<div class=\"trace\">"
                "<div class=\"trace-head\">"
                    "<span class=\"trace-name\">Geophone 4</span>"
                    "<span id=\"range4\" class=\"muted\">--</span>"
                "</div>"
                "<canvas id=\"graph4\"></canvas>"
            "</div>"

        "</main>"

        "<script>"
            "const POINTS=240;"
            "let lastData=null;"

            "function el(id){"
                "return document.getElementById(id);"
            "}"

            "function setStatus(text,cls){"
                "el('status').textContent=text;"
                "el('status').className='status '+cls;"
            "}"

            "function scaleFor(data){"
                "let max=0;"

                "data.channels.forEach(function(channel){"
                    "channel.samples_uv.forEach(function(value){"
                        "max=Math.max(max,Math.abs(value));"
                    "});"
                "});"

                "return Math.max(10,max*1.10);"
            "}"

            "function drawGraph(id,samples,scale){"
                "const canvas=el(id);"
                "const ratio=window.devicePixelRatio||1;"
                "const width=Math.max(280,canvas.clientWidth);"
                "const height=120;"

                "canvas.width=Math.floor(width*ratio);"
                "canvas.height=Math.floor(height*ratio);"

                "const ctx=canvas.getContext('2d');"
                "ctx.setTransform(ratio,0,0,ratio,0,0);"

                "ctx.clearRect(0,0,width,height);"

                "ctx.strokeStyle='#21262d';"
                "ctx.lineWidth=1;"

                "for(let i=1;i<4;i++){"
                    "const y=height*i/4;"
                    "ctx.beginPath();"
                    "ctx.moveTo(0,y);"
                    "ctx.lineTo(width,y);"
                    "ctx.stroke();"
                "}"

                "ctx.strokeStyle='#30363d';"
                "ctx.beginPath();"
                "ctx.moveTo(0,height/2);"
                "ctx.lineTo(width,height/2);"
                "ctx.stroke();"

                "if(samples.length<2){"
                    "return;"
                "}"

                "ctx.strokeStyle='#58a6ff';"
                "ctx.lineWidth=1.5;"
                "ctx.beginPath();"

                "samples.forEach(function(value,index){"
                    "const x="
                        "index/(samples.length-1)*width;"

                    "const y="
                        "height/2-"
                        "(value/scale)*(height*0.42);"

                    "if(index===0){"
                        "ctx.moveTo(x,y);"
                    "}else{"
                        "ctx.lineTo(x,y);"
                    "}"
                "});"

                "ctx.stroke();"
            "}"

            "function render(data){"
                "lastData=data;"

                "el('source').textContent=data.source;"
                "el('rate').textContent="
                    "data.sample_rate_hz+' Hz';"

                "const seconds="
                    "data.sample_rate_hz>0?"
                    "data.count/data.sample_rate_hz:"
                    "0;"

                "el('window').textContent="
                    "seconds.toFixed(1)+' s';"

                "el('frames').textContent="
                    "data.total_frames;"

                "if(data.source==='TEST'){"
                    "setStatus('TEST SIGNAL','warning');"
                "}else if(data.source==='AD7606'){"
                    "setStatus('LIVE','ok');"
                "}else{"
                    "setStatus('WAITING','muted');"
                "}"

                "const scale=scaleFor(data);"

                "data.channels.forEach(function(channel,index){"
                    "drawGraph("
                        "'graph'+channel.id,"
                        "channel.samples_uv,"
                        "scale"
                    ");"

                    "el('range'+channel.id).textContent="
                        "'+/- '+scale.toFixed(1)+' uV';"
                "});"
            "}"

            "function refresh(){"
                "fetch("
                    "'/api/seismic?points='+POINTS,"
                    "{cache:'no-store'}"
                ")"
                ".then(function(response){"
                    "if(!response.ok){"
                        "throw new Error();"
                    "}"
                    "return response.json();"
                "})"
                ".then(function(data){"
                    "render(data);"
                "})"
                ".catch(function(){"
                    "setStatus('OFFLINE','error');"
                "});"
            "}"

            "window.addEventListener("
                "'resize',"
                "function(){"
                    "if(lastData){"
                        "render(lastData);"
                    "}"
                "}"
            ");"

            "refresh();"
            "setInterval(refresh,500);"
        "</script>"

    "</body>"
    "</html>";


const char *web_seismic_page_get_html(void)
{
    return s_html;
}