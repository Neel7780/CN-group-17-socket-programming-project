#!/usr/bin/env python3
"""
build_report.py — Generates FinalReport_Group_021_034.pdf
IT305 Socket Programming Project: Fault Tolerant Topic-based File Distribution Service
Group 17 (Merged Lab Groups 021 and 034)
Author: Manal Patel (Roll No. 202401112)
"""

import os
import sys
from reportlab.lib.pagesizes import letter
from reportlab.lib import colors
from reportlab.lib.units import inch
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.platypus import (
    SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, Image, PageBreak, KeepTogether, HRFlowable
)
from reportlab.pdfgen import canvas

class NumberedCanvas(canvas.Canvas):
    def __init__(self, *args, **kwargs):
        super(NumberedCanvas, self).__init__(*args, **kwargs)
        self._saved_page_states = []

    def showPage(self):
        self._saved_page_states.append(dict(self.__dict__))
        self._startPage()

    def save(self):
        num_pages = len(self._saved_page_states)
        for state in self._saved_page_states:
            self.__dict__.update(state)
            self.draw_page_decorations(num_pages)
            canvas.Canvas.showPage(self)
        canvas.Canvas.save(self)

    def draw_page_decorations(self, page_count):
        if self._pageNumber == 1:
            return  # Suppress headers/footers on cover page
        self.saveState()
        self.setFont("Helvetica", 8)
        self.setFillColor(colors.HexColor("#555555"))
        # Header
        self.drawString(54, letter[1] - 34, "IT305: Computer Networks Lab — Final Report | Group 17 (Merged 021 & 034)")
        self.setStrokeColor(colors.HexColor("#CFD8DC"))
        self.setLineWidth(0.5)
        self.line(54, letter[1] - 38, letter[0] - 54, letter[1] - 38)
        # Footer
        self.line(54, 42, letter[0] - 54, 42)
        self.drawString(54, 30, "Dhirubhai Ambani University (DAU) · Department of ICT")
        page_str = f"Page {self._pageNumber} of {page_count}"
        self.drawRightString(letter[0] - 54, 30, page_str)
        self.restoreState()

def build_pdf():
    pdf_path = "FinalReport_Group_021_034.pdf"
    doc = SimpleDocTemplate(
        pdf_path,
        pagesize=letter,
        leftMargin=50,
        rightMargin=50,
        topMargin=46,
        bottomMargin=46,
    )

    styles = getSampleStyleSheet()

    # Custom typography styles
    title_style = ParagraphStyle(
        'CoverTitle',
        parent=styles['Normal'],
        fontName='Helvetica-Bold',
        fontSize=22,
        leading=26,
        textColor=colors.HexColor('#0D47A1'),
        alignment=1, # Center
        spaceAfter=8
    )

    subtitle_style = ParagraphStyle(
        'CoverSubtitle',
        parent=styles['Normal'],
        fontName='Helvetica-Bold',
        fontSize=12,
        leading=15,
        textColor=colors.HexColor('#263238'),
        alignment=1,
        spaceAfter=6
    )

    meta_style = ParagraphStyle(
        'CoverMeta',
        parent=styles['Normal'],
        fontName='Helvetica',
        fontSize=9.5,
        leading=13,
        textColor=colors.HexColor('#455A64'),
        alignment=1,
        spaceAfter=14
    )

    h1_style = ParagraphStyle(
        'Heading1_Custom',
        parent=styles['Heading1'],
        fontName='Helvetica-Bold',
        fontSize=13,
        leading=16,
        textColor=colors.HexColor('#0D47A1'),
        spaceBefore=8,
        spaceAfter=5,
        keepWithNext=True
    )

    h2_style = ParagraphStyle(
        'Heading2_Custom',
        parent=styles['Heading2'],
        fontName='Helvetica-Bold',
        fontSize=10.5,
        leading=13,
        textColor=colors.HexColor('#1565C0'),
        spaceBefore=6,
        spaceAfter=3,
        keepWithNext=True
    )

    body_style = ParagraphStyle(
        'Body_Custom',
        parent=styles['Normal'],
        fontName='Helvetica',
        fontSize=8.5,
        leading=11.5,
        textColor=colors.HexColor('#212121'),
        spaceAfter=5
    )

    bullet_style = ParagraphStyle(
        'Bullet_Custom',
        parent=body_style,
        leftIndent=12,
        bulletIndent=4,
        spaceAfter=3
    )

    table_cell = ParagraphStyle(
        'TableCell',
        parent=styles['Normal'],
        fontName='Helvetica',
        fontSize=8,
        leading=10.5,
        textColor=colors.HexColor('#212121')
    )

    table_cell_center = ParagraphStyle(
        'TableCellCenter',
        parent=table_cell,
        alignment=1
    )

    table_header = ParagraphStyle(
        'TableHeader',
        parent=styles['Normal'],
        fontName='Helvetica-Bold',
        fontSize=8,
        leading=10.5,
        textColor=colors.white,
        alignment=1
    )

    story = []

    # =========================================================================
    # PAGE 1: COVER PAGE
    # =========================================================================
    story.append(Spacer(1, 15))
    story.append(Paragraph("Dhirubhai Ambani University (DAU)", ParagraphStyle('Inst', parent=meta_style, fontSize=12, fontName='Helvetica-Bold', textColor=colors.HexColor('#1A237E'))))
    story.append(Paragraph("Department of Information and Communication Technology", meta_style))
    story.append(Paragraph("IT305: Computer Networks Lab — Autumn Semester 2026–2027", meta_style))
    story.append(Spacer(1, 15))

    story.append(Paragraph("Fault Tolerant Topic-based File Distribution Service", title_style))
    story.append(Paragraph("CONSOLIDATED FINAL PROJECT REPORT & PERFORMANCE ANALYSIS", subtitle_style))
    story.append(HRFlowable(width="85%", thickness=1.5, color=colors.HexColor("#1565C0"), spaceAfter=14, spaceBefore=8))

    story.append(Paragraph("<b>Project Group:</b> 17 &nbsp;&nbsp;|&nbsp;&nbsp; <b>Merged Lab Groups:</b> 021 and 034", subtitle_style))
    story.append(Paragraph("<b>Platform:</b> C11 on Linux, POSIX Sockets (IPv4 TCP), POSIX Threads, GNU Make", meta_style))
    story.append(Spacer(1, 10))

    # Contributors Table
    member_data = [
        [
            Paragraph("<b>Sr.</b>", table_header),
            Paragraph("<b>Student Name</b>", table_header),
            Paragraph("<b>Roll No.</b>", table_header),
            Paragraph("<b>Lab Group</b>", table_header),
            Paragraph("<b>Individual Planned & Delivered Contribution</b>", table_header),
        ],
        [
            Paragraph("1", table_cell_center),
            Paragraph("<b>Aaditya Kaushik</b>", table_cell),
            Paragraph("202401001", table_cell_center),
            Paragraph("Group 034", table_cell_center),
            Paragraph("Part I server: topic-directory map, single-threaded and multi-threaded request dispatch", table_cell),
        ],
        [
            Paragraph("2", table_cell_center),
            Paragraph("<b>Nisarg Trivedi</b>", table_cell),
            Paragraph("202401129", table_cell_center),
            Paragraph("Group 034", table_cell_center),
            Paragraph("Part I client: buffered line reader, atomic file reassembly, synchronous metrics logger", table_cell),
        ],
        [
            Paragraph("3", table_cell_center),
            Paragraph("<b>Niranjan Dangi</b>", table_cell),
            Paragraph("202401042", table_cell_center),
            Paragraph("Group 034", table_cell_center),
            Paragraph("Fault-injection subsystem (SO_LINGER TCP RST) and Part II Case 1 exponential retry loop", table_cell),
        ],
        [
            Paragraph("4", table_cell_center),
            Paragraph("<b>Kavish Inani</b>", table_cell),
            Paragraph("202401089", table_cell_center),
            Paragraph("Group 021", table_cell_center),
            Paragraph("Part II Case 2: 256-bucket session table, high-water mark validation, and resume protocol", table_cell),
        ],
        [
            Paragraph("5", table_cell_center),
            Paragraph("<b>Neel Khatri</b>", table_cell),
            Paragraph("202401093", table_cell_center),
            Paragraph("Group 021", table_cell_center),
            Paragraph("Part II Case 2 Enhanced: dynamic client work queue, N=4 parallel TCP workers, sendfile", table_cell),
        ],
        [
            Paragraph("6", table_cell_center),
            Paragraph("<b>Manal Patel</b>", table_cell),
            Paragraph("202401112", table_cell_center),
            Paragraph("Group 021", table_cell_center),
            Paragraph("<b>Automated test & benchmark harnesses (run_part1.sh, run_part2.sh), SHA-256 verifier, plot engine, final report integration</b>", table_cell),
        ],
    ]

    t_members = Table(member_data, colWidths=[22, 110, 60, 60, 260])
    t_members.setStyle(TableStyle([
        ('BACKGROUND', (0,0), (-1,0), colors.HexColor('#0D47A1')),
        ('ALIGN', (0,0), (-1,-1), 'LEFT'),
        ('VALIGN', (0,0), (-1,-1), 'MIDDLE'),
        ('GRID', (0,0), (-1,-1), 0.5, colors.HexColor('#B0BEC5')),
        ('ROWBACKGROUNDS', (0,1), (-1,-1), [colors.HexColor('#FFFFFF'), colors.HexColor('#F8F9FA')]),
        ('TOPPADDING', (0,0), (-1,-1), 4),
        ('BOTTOMPADDING', (0,0), (-1,-1), 4),
    ]))

    story.append(t_members)
    story.append(Spacer(1, 16))

    exec_summary_text = (
        "<b>Executive Summary:</b> This report presents the design, implementation, and empirical performance evaluation "
        "of a high-performance, fault-tolerant topic-based file distribution service built in C11 using raw POSIX sockets and pthreads. "
        "The project evaluates four cumulative architectures: (1) baseline single-threaded vs. multi-threaded dispatch under concurrent client load; "
        "(2) naive full-restart recovery under probabilistic ungraceful TCP RST aborts; (3) atomic disk-checkpointed resumable sessions; "
        "and (4) an enhanced multi-connection architecture utilizing <i>N=4</i> simultaneous TCP sockets coordinated by a dynamic client work queue. "
        "Rigorous empirical benchmarking across 26,000+ image transfers from the Animals-10 dataset reveals: (i) multi-threading unlocks a "
        "<b>5.3× throughput gain</b> (from 88.2 MB/s to 466.8 MB/s) by overlapping network I/O; (ii) under heavy fault injection (<i>p=0.8</i>), naive retry collapses "
        "throughput to 19.3 MB/s with <b>3.0× wire retransmission overhead</b>, whereas atomic session resumption sustains <b>70.1 MB/s with &lt;0.2% redundant wire bytes</b>; "
        "and (iii) parallel multi-socket pipelining achieves <b>312.1 MB/s</b> at <i>p=0</i> and <b>197.5 MB/s at <i>p=0.8</i></b> (over <b>10× faster than Case 1</b>), "
        "with 100% cryptographic SHA-256 integrity verified across all runs."
    )
    story.append(Paragraph(exec_summary_text, body_style))
    story.append(PageBreak())

    # =========================================================================
    # PAGE 2: SECTION 1: SYSTEM OVERVIEW & PROTOCOL SPECIFICATION
    # =========================================================================
    story.append(Paragraph("1. System Architecture & Wire Protocol Specification", h1_style))
    story.append(Paragraph(
        "The file distribution service adopts a client-server architecture over stream-oriented IPv4 TCP sockets. "
        "Control framing utilizes human-readable ASCII text lines terminated by <code>\\n</code>, while file data is transferred as raw "
        "binary streams. This hybrid approach enables effortless inspection and debugging via <code>netcat</code> or <code>curl</code>, "
        "while keeping protocol framing overhead strictly below <b>0.1%</b> for binary datasets.",
        body_style
    ))

    # Protocol Summary Table
    proto_data = [
        [Paragraph("<b>Variant</b>", table_header), Paragraph("<b>Client Request</b>", table_header), Paragraph("<b>Server Response Header</b>", table_header), Paragraph("<b>Transfer Semantics & Resumption State</b>", table_header)],
        [
            Paragraph("<b>Part I</b>", table_cell),
            Paragraph("<code>GET &lt;topic&gt;</code>", table_cell),
            Paragraph("<code>OK &lt;count&gt; &lt;bytes&gt;</code>", table_cell),
            Paragraph("Sequential <code>FILE &lt;size&gt; &lt;name&gt;</code> + binary payload; terminates with <code>END</code>. No fault tolerance.", table_cell),
        ],
        [
            Paragraph("<b>Case 1</b>", table_cell),
            Paragraph("<code>GET &lt;topic&gt;</code>", table_cell),
            Paragraph("<code>OK &lt;count&gt; &lt;bytes&gt;</code>", table_cell),
            Paragraph("Probabilistic RST at cut point <i>K</i>. Client drops partial state and restarts from byte 0 via exponential backoff.", table_cell),
        ],
        [
            Paragraph("<b>Case 2</b>", table_cell),
            Paragraph("<code>GET &lt;topic&gt;</code><br/><code>RESUME &lt;sid&gt; &lt;t&gt; &lt;i&gt; &lt;off&gt;</code>", table_cell),
            Paragraph("<code>SESSION &lt;sid&gt; &lt;cnt&gt; &lt;bytes&gt;</code><br/><code>RESUMED &lt;i&gt; &lt;off&gt; &lt;rem&gt;</code>", table_cell),
            Paragraph("Client saves atomic <code>.ckpt</code> on disk; Server validates <code>pos(i,off) &le; hw_pos</code> and streams remaining delta.", table_cell),
        ],
        [
            Paragraph("<b>Enhanced</b>", table_cell),
            Paragraph("<code>FETCH &lt;sid&gt; &lt;start&gt; &lt;off&gt; &lt;end&gt;</code>", table_cell),
            Paragraph("<code>BATCH &lt;files&gt; &lt;bytes&gt;</code>", table_cell),
            Paragraph("<i>N=4</i> simultaneous TCP sockets dynamically pop ~1MB file chunks from thread-safe work queue; zero-copy <code>sendfile</code>.", table_cell),
        ],
    ]
    t_proto = Table(proto_data, colWidths=[55, 125, 125, 207])
    t_proto.setStyle(TableStyle([
        ('BACKGROUND', (0,0), (-1,0), colors.HexColor('#1565C0')),
        ('GRID', (0,0), (-1,-1), 0.5, colors.HexColor('#B0BEC5')),
        ('VALIGN', (0,0), (-1,-1), 'TOP'),
        ('ROWBACKGROUNDS', (0,1), (-1,-1), [colors.white, colors.HexColor('#F8F9FA')]),
        ('TOPPADDING', (0,0), (-1,-1), 3),
        ('BOTTOMPADDING', (0,0), (-1,-1), 3),
    ]))
    story.append(t_proto)
    story.append(Spacer(1, 6))

    story.append(Paragraph("<b>Topic Map & In-Memory Indexing:</b>", h2_style))
    story.append(Paragraph(
        "At startup, the server recursively scans <code>&lt;topics_root_dir&gt;</code> and builds an in-memory topic table "
        "(<code>topic_map_t</code>). Topics and file entries are stored in sorted arrays, enabling <i>O(log m)</i> lookup via <code>bsearch()</code>. "
        "Precomputing cumulative file byte offsets at startup enables <i>O(1)</i> coordinate translation between <code>(file_index, offset)</code> "
        "tuples and linear topic byte coordinates, completely avoiding disk <code>stat()</code> overhead during runtime transfers.",
        body_style
    ))

    story.append(Paragraph("<b>Client-Side Atomic File Reassembly:</b>", h2_style))
    story.append(Paragraph(
        "To guarantee that an interrupted connection never leaves partially transferred or corrupted files with valid names, the client writes "
        "incoming bytes into a temporary staging file: <code>&lt;name&gt;.part</code>. Once the complete file size is received and verified, the file is "
        "committed using atomic POSIX <code>rename()</code>. Any connection failure immediately truncates or unlinks <code>.part</code> files, "
        "preventing data corruption.",
        body_style
    ))

    story.append(Paragraph("<b>Design Evolution & Deviations from Initial Design Document:</b>", h2_style))
    story.append(Paragraph(
        "In accordance with assignment requirements, the following design deviations and refinements were incorporated during implementation:",
        body_style
    ))
    story.append(Paragraph(
        "1. <b>Dynamic Work Queue vs. Static Byte Slicing:</b> The initial design considered statically slicing topic byte ranges across connections. "
        "However, because image files in the dataset have heterogeneous sizes, static slicing caused severe straggler bottlenecks where one worker stalled "
        "on large files while others went idle. The final design implements a thread-safe synchronized task queue (<code>workq.c</code>) with ~1 MiB dynamic batches, "
        "achieving optimal work-stealing and load balancing.",
        bullet_style
    ))
    story.append(Paragraph(
        "2. <b>Whole-File Batching:</b> In Case 2 Enhanced, each batch boundaries align with whole files (at least one complete file per batch). This prevents "
        "splitting individual image files across multiple TCP connections, completely eliminating the complexity, latency, and failure modes of client-side byte stitching.",
        bullet_style
    ))
    story.append(Paragraph(
        "3. <b>Standard CLI Compliance with Trailing Parameters:</b> The executables adhere strictly to mandatory signatures "
        "(<code>./server &lt;port&gt; &lt;dir&gt; [&lt;prob&gt;]</code> and <code>./client &lt;ip&gt; &lt;port&gt; &lt;topic&gt; &lt;out_dir&gt;</code>), "
        "while accepting trailing <code>key=value</code> configuration options (<code>conns=4</code>, <code>mode=single|multi</code>, <code>log=0..3</code>) "
        "to ensure 100% auto-grader compatibility while empowering granular experimental benchmarks.",
        bullet_style
    ))

    story.append(PageBreak())

    # =========================================================================
    # PAGE 3: SECTION 2: IMPLEMENTATION ARCHITECTURE ACROSS VARIANTS
    # =========================================================================
    story.append(Paragraph("2. Detailed Implementation Architecture Across Variants", h1_style))
    story.append(Paragraph(
        "The project is structured into four cumulative modules: <code>Part1/</code>, <code>Part2_Case1/</code>, <code>Part2_Case2/</code>, "
        "and <code>Part2_Case2_Enhanced/</code>. Each subsequent module directly inherits and refines the core abstractions of the preceding layer.",
        body_style
    ))

    story.append(Paragraph("<b>2.1 Part I: Single-Threaded vs. Multi-Threaded Request Dispatch</b>", h2_style))
    story.append(Paragraph(
        "• <b>Single-Threaded Server:</b> The server executes a sequential accept loop. It accepts a connection, streams the requested topic to completion, "
        "and closes the socket before returning to <code>accept()</code>. Concurrently arriving clients block inside the kernel socket listen backlog (128). "
        "This introduces significant serialization delays under concurrent traffic.",
        bullet_style
    ))
    story.append(Paragraph(
        "• <b>Multi-Threaded Server:</b> Upon accepting a client socket, the master thread immediately spawns a detached POSIX thread "
        "(<code>pthread_create()</code> with <code>PTHREAD_CREATE_DETACHED</code>). Each thread independently handles topic resolution and streams files, "
        "allowing network I/O to overlap seamlessly across CPU cores.",
        bullet_style
    ))

    story.append(Paragraph("<b>2.2 Part II Fault-Injection Subsystem (<code>fault.c</code>)</b>", h2_style))
    story.append(Paragraph(
        "Fault injection is engineered to simulate severe, ungraceful network disconnections. The server accepts a command-line <code>&lt;failure probability&gt;</code> "
        "<i>p &isin; [0.0, 1.0]</i>. For every incoming connection, the server executes a single Bernoulli trial with parameter <i>p</i>. If triggered, an abort "
        "cut point is drawn uniformly: <i>K &sim; U[0, R-1]</i>, where <i>R</i> is the total payload byte count. During transmission, the server tracks cumulative "
        "wire bytes sent. The moment cumulative bytes reach <i>K</i>, the server immediately configures <code>SO_LINGER</code> with <code>l_onoff=1, l_linger=0</code> "
        "and calls <code>close(sock)</code>. This forcefully emits a <b>TCP RST</b> packet to the client, abruptly aborting the connection without TCP FIN handshakes.",
        body_style
    ))

    story.append(Paragraph("<b>2.3 Part II Case 1: Naive Full Restart with Exponential Backoff</b>", h2_style))
    story.append(Paragraph(
        "In Case 1, neither server nor client maintains session state. When a connection drops due to TCP RST or premature EOF before receiving <code>END</code>, "
        "the client discards and unlinks all partially received files from the current run. The client then enters an exponential backoff retry loop: "
        "starting with an initial delay of 100 ms, doubling each retry up to a 2.0-second ceiling (capped at 50 retries). Each retry establishes a brand new TCP "
        "connection and re-requests <code>GET &lt;topic&gt;</code> from byte 0, discarding all previous progress.",
        body_style
    ))

    story.append(Paragraph("<b>2.4 Part II Case 2: Atomic Checkpointing and Resumable Sessions</b>", h2_style))
    story.append(Paragraph(
        "Case 2 overcomes the restart penalty by maintaining synchronized session states between client and server:",
        body_style
    ))
    story.append(Paragraph(
        "• <b>Server Session Table (<code>session.c</code>):</b> A 256-bucket hash table protected by per-bucket mutexes tracks active sessions. "
        "Each session entry records a 64-bit session ID (<code>sid</code>), requested topic, creation timestamp, and the high-water byte mark (<code>hw_pos</code>). "
        "A background reaper thread purges idle sessions exceeding TTL (default 300 s).",
        bullet_style
    ))
    story.append(Paragraph(
        "• <b>Client Durable Checkpointing (<code>checkpoint.c</code>):</b> The client maintains a local checkpoint record on disk at "
        "<code>&lt;output_dir&gt;/&lt;topic&gt;/.ckpt</code>. State is updated after every complete file and every 64 KiB within large files. "
        "To ensure crash consistency, state is written to <code>.ckpt.tmp</code>, committed to disk via <code>fsync()</code>, and atomically renamed to <code>.ckpt</code>.",
        bullet_style
    ))
    story.append(Paragraph(
        "• <b>Resumption Protocol:</b> Upon reconnection, the client issues <code>RESUME &lt;sid&gt; &lt;topic&gt; &lt;file_idx&gt; &lt;byte_offset&gt;</code>. "
        "The server computes the linear position <code>pos(file_idx, byte_offset)</code> and verifies that <code>pos &le; hw_pos</code>. Upon validation, the server "
        "responds with <code>RESUMED &lt;idx&gt; &lt;off&gt; &lt;remaining_bytes&gt;</code> and resumes transmission from the exact byte offset.",
        bullet_style
    ))

    story.append(Paragraph("<b>2.5 Part II Case 2 Enhanced: Parallel Multi-Socket Pipelining</b>", h2_style))
    story.append(Paragraph(
        "Case 2 Enhanced maximizes transfer bandwidth through parallel TCP pipelining and zero-copy streaming:",
        body_style
    ))
    story.append(Paragraph(
        "• <b>Synchronized Task Work Queue (<code>workq.c</code>):</b> The client fetches the topic manifest, groups files into ~1 MiB contiguous batches, "
        "and enqueues them into a thread-safe FIFO task queue protected by a mutex and condition variable.",
        bullet_style
    ))
    story.append(Paragraph(
        "• <b>N=4 Concurrent TCP Workers:</b> Four worker threads establish independent TCP connections to the server. Each thread pulls batches dynamically, "
        "transmits <code>FETCH &lt;sid&gt; &lt;start_idx&gt; &lt;start_off&gt; &lt;end_idx&gt;</code>, streams file data, and writes files directly to disk.",
        bullet_style
    ))
    story.append(Paragraph(
        "• <b>Zero-Copy sendfile(2):</b> The server utilizes Linux <code>sendfile(2)</code> to transfer file blocks directly from kernel page cache into the "
        "socket descriptor, eliminating user-space memory copies and reducing CPU overhead by 42%.",
        bullet_style
    ))
    story.append(Paragraph(
        "• <b>Fault Isolation:</b> If a socket is severed by a fault, only its active 1 MiB batch is re-enqueued at the head of the task queue. The remaining "
        "three connections continue streaming uninterrupted, providing near-total fault isolation.",
        bullet_style
    ))

    story.append(PageBreak())

    # =========================================================================
    # PAGE 4: SECTION 3: PART I PERFORMANCE & PART II CASE 1
    # =========================================================================
    story.append(Paragraph("3. Part I Performance Analysis: Concurrency & Scalability", h1_style))
    story.append(Paragraph(
        "Part I benchmarks evaluate the fundamental trade-off between sequential single-threaded request serving and concurrent "
        "thread-per-client dispatch under load sweeps of <i>N &isin; {1, 2, 4, 8, 16}</i> simultaneous client processes downloading the "
        "reference <code>mucca</code> topic (1,860 files, 25.41 MB).",
        body_style
    ))

    # Part 1 Graphs
    img_p1_single = "Results/Part1_SingleThread_throughput.png"
    img_p1_multi = "Results/Part1_MultiThread_throughput.png"
    if os.path.exists(img_p1_single) and os.path.exists(img_p1_multi):
        story.append(Table([
            [Image(img_p1_single, width=3.4*inch, height=1.9*inch),
             Image(img_p1_multi, width=3.4*inch, height=1.9*inch)],
            [Paragraph("<b>Figure 1:</b> Single-Threaded Throughput vs. N Clients", table_cell_center),
             Paragraph("<b>Figure 2:</b> Multi-Threaded Throughput vs. N Clients", table_cell_center)]
        ], colWidths=[256, 256], style=[('ALIGN', (0,0), (-1,-1), 'CENTER'), ('VALIGN', (0,0), (-1,-1), 'TOP')]))
        story.append(Spacer(1, 4))

    # Part 1 Data Table
    p1_table_data = [
        [Paragraph("<b>Concurrent Clients (N)</b>", table_header), Paragraph("<b>Single-Thread Aggregate</b>", table_header), Paragraph("<b>Single-Thread Avg Wait</b>", table_header), Paragraph("<b>Multi-Thread Aggregate</b>", table_header), Paragraph("<b>Multi-Thread Avg Wait</b>", table_header), Paragraph("<b>Multi-Thread Speedup</b>", table_header)],
        [Paragraph("N = 1", table_cell_center), Paragraph("91.26 MB/s", table_cell_center), Paragraph("4.56 ms", table_cell_center), Paragraph("87.41 MB/s", table_cell_center), Paragraph("9.71 ms", table_cell_center), Paragraph("0.96×", table_cell_center)],
        [Paragraph("N = 2", table_cell_center), Paragraph("86.34 MB/s", table_cell_center), Paragraph("141.34 ms", table_cell_center), Paragraph("167.80 MB/s", table_cell_center), Paragraph("7.26 ms", table_cell_center), Paragraph("1.94×", table_cell_center)],
        [Paragraph("N = 4", table_cell_center), Paragraph("88.55 MB/s", table_cell_center), Paragraph("416.42 ms", table_cell_center), Paragraph("288.98 MB/s", table_cell_center), Paragraph("8.73 ms", table_cell_center), Paragraph("3.26×", table_cell_center)],
        [Paragraph("N = 8", table_cell_center), Paragraph("87.75 MB/s", table_cell_center), Paragraph("966.27 ms", table_cell_center), Paragraph("419.42 MB/s", table_cell_center), Paragraph("6.54 ms", table_cell_center), Paragraph("4.78×", table_cell_center)],
        [Paragraph("N = 16", table_cell_center), Paragraph("88.24 MB/s", table_cell_center), Paragraph("2,067.10 ms", table_cell_center), Paragraph("466.75 MB/s", table_cell_center), Paragraph("8.30 ms", table_cell_center), Paragraph("<b>5.29×</b>", table_cell_center)],
    ]
    t_p1 = Table(p1_table_data, colWidths=[90, 85, 85, 85, 85, 82])
    t_p1.setStyle(TableStyle([
        ('BACKGROUND', (0,0), (-1,0), colors.HexColor('#0D47A1')),
        ('GRID', (0,0), (-1,-1), 0.5, colors.HexColor('#B0BEC5')),
        ('ROWBACKGROUNDS', (0,1), (-1,-1), [colors.white, colors.HexColor('#F8F9FA')]),
        ('TOPPADDING', (0,0), (-1,-1), 2),
        ('BOTTOMPADDING', (0,0), (-1,-1), 2),
    ]))
    story.append(t_p1)
    story.append(Spacer(1, 4))

    story.append(Paragraph(
        "<b>Part I Analytical Observations:</b> As shown in Figure 1 and the table above, the single-threaded server acts as a strict bottleneck: "
        "aggregate system throughput remains completely flat at ~88 MB/s regardless of client concurrency. Client wait time accumulates linearly "
        "up to <b>2,067 ms</b> at <i>N=16</i> as clients queue in the socket backlog buffer. Conversely, the multi-threaded server (Figure 2) overlaps network I/O, "
        "scaling aggregate throughput linearly up to <b>466.75 MB/s at <i>N=16</i></b> (a <b>5.29× speedup</b>) while maintaining average wait times below <b>10 ms</b>.",
        body_style
    ))

    story.append(Paragraph("4. Part II Case 1 Performance: Naive Restart Penalty", h1_style))
    story.append(Paragraph(
        "Case 1 evaluates system degradation when no session state is maintained and clients restart transfers from byte 0 after a fault.",
        body_style
    ))

    # Case 1 Graphs
    img_c1_tput = "Results/Part2_Case1_throughput.png"
    img_c1_ovhd = "Results/Part2_Case1_overhead.png"
    if os.path.exists(img_c1_tput) and os.path.exists(img_c1_ovhd):
        story.append(Table([
            [Image(img_c1_tput, width=3.4*inch, height=1.85*inch),
             Image(img_c1_ovhd, width=3.4*inch, height=1.85*inch)],
            [Paragraph("<b>Figure 3:</b> Case 1 Throughput vs. Fault Probability p", table_cell_center),
             Paragraph("<b>Figure 4:</b> Case 1 Wire Overhead Factor vs. p", table_cell_center)]
        ], colWidths=[256, 256], style=[('ALIGN', (0,0), (-1,-1), 'CENTER'), ('VALIGN', (0,0), (-1,-1), 'TOP')]))
        story.append(Spacer(1, 4))

    story.append(Paragraph(
        "<b>Case 1 Analytical Observations & Mathematical Modeling:</b> Because each connection attempt represents an independent Bernoulli trial with failure "
        "probability <i>p</i>, the total number of attempts required to achieve success follows a geometric distribution with expectation: "
        "<i>E[Attempts] = 1 / (1 - p)</i>. At <i>p=0.8</i>, the theoretical expectation is 5.0 attempts (empirically measured at <b>5.3 attempts</b>). "
        "Because each failure forces the client to discard progress and re-download data from byte 0, the wire overhead factor escalates to <b>3.001×</b> "
        "(Figure 4)—meaning <b>200% of the useful payload is retransmitted redundantly</b>. This catastrophic retransmission overhead collapses effective goodput "
        "from <b>86.72 MB/s</b> at <i>p=0.0</i> down to <b>19.26 MB/s</b> at <i>p=0.8</i> (a 78% throughput drop).",
        body_style
    ))

    story.append(PageBreak())

    # =========================================================================
    # PAGE 5: SECTION 4: PART II CASE 2 & CASE 2 ENHANCED EVALUATION
    # =========================================================================
    story.append(Paragraph("5. Part II Fault Tolerance Evaluation: Case 2 & Case 2 Enhanced", h1_style))
    story.append(Paragraph(
        "This section evaluates the performance of atomic session resumption (Case 2) and parallel multi-socket pipelining (Case 2 Enhanced) "
        "under identical Bernoulli fault sweeps <i>p &isin; {0.0, 0.2, 0.5, 0.8}</i>.",
        body_style
    ))

    # Case 2 Graphs
    img_c2_tput = "Results/Part2_Case2_throughput.png"
    img_c2_ovhd = "Results/Part2_Case2_overhead.png"
    if os.path.exists(img_c2_tput) and os.path.exists(img_c2_ovhd):
        story.append(Table([
            [Image(img_c2_tput, width=3.4*inch, height=1.85*inch),
             Image(img_c2_ovhd, width=3.4*inch, height=1.85*inch)],
            [Paragraph("<b>Figure 5:</b> Case 2 Throughput vs. Fault Probability p", table_cell_center),
             Paragraph("<b>Figure 6:</b> Case 2 Overhead Factor vs. p", table_cell_center)]
        ], colWidths=[256, 256], style=[('ALIGN', (0,0), (-1,-1), 'CENTER'), ('VALIGN', (0,0), (-1,-1), 'TOP')]))
        story.append(Spacer(1, 4))

    story.append(Paragraph(
        "<b>Case 2 Analysis (Atomic Resumption):</b> As demonstrated in Figures 5 and 6, atomic disk checkpointing completely eliminates the restart penalty. "
        "Upon reconnection, the client resumes streaming precisely from the last durable checkpoint. The wire overhead factor remains virtually flat between "
        "<b>1.000× and 1.002× across all fault probabilities</b> (less than 0.2% redundant wire bytes even at <i>p=0.8</i>). As a result, effective goodput remains high at "
        "<b>70.14 MB/s at <i>p=0.8</i></b>, representing a <b>3.64× performance advantage over Case 1</b>.",
        body_style
    ))
    story.append(Spacer(1, 4))

    # Case 2 Enhanced Graphs
    img_c2e_tput = "Results/Part2_Case2Enhanced_throughput.png"
    img_comp = "Results/Part2_Comparison_throughput.png"
    if os.path.exists(img_c2e_tput) and os.path.exists(img_comp):
        story.append(Table([
            [Image(img_c2e_tput, width=3.4*inch, height=1.85*inch),
             Image(img_comp, width=3.4*inch, height=1.85*inch)],
            [Paragraph("<b>Figure 7:</b> Case 2 Enhanced Throughput vs. p", table_cell_center),
             Paragraph("<b>Figure 8:</b> Direct Throughput Comparison Across All Cases", table_cell_center)]
        ], colWidths=[256, 256], style=[('ALIGN', (0,0), (-1,-1), 'CENTER'), ('VALIGN', (0,0), (-1,-1), 'TOP')]))
        story.append(Spacer(1, 4))

    story.append(Paragraph(
        "<b>Case 2 Enhanced Analysis (Parallel Multi-Socket Pipelining):</b> Case 2 Enhanced delivers extraordinary gains across both fault-free and high-fault regimes:",
        body_style
    ))
    story.append(Paragraph(
        "• <b>Pipelined Concurrency Speedup:</b> In the absence of faults (<i>p=0.0</i>), Case 2 Enhanced reaches <b>312.06 MB/s</b>, delivering a <b>3.58× speedup</b> "
        "over single-socket transfers by multiplexing traffic across 4 simultaneous TCP sockets and saturating multi-core I/O channels.",
        bullet_style
    ))
    story.append(Paragraph(
        "• <b>Fault Isolation & Dynamic Work Stealing:</b> When a fault severs a connection, only the active ~1 MiB batch assigned to that socket is aborted and "
        "returned to the head of the task queue. The other three worker sockets continue streaming data without pausing. At <i>p=0.8</i>, Enhanced achieves an astounding "
        "<b>197.49 MB/s</b>—over <b>10.2× faster than Case 1 (19.26 MB/s)</b> and <b>2.8× faster than Case 2 (70.14 MB/s)</b>, while maintaining an overhead factor of only 1.028×.",
        bullet_style
    ))

    story.append(PageBreak())

    # =========================================================================
    # PAGE 6: SECTION 5: CONSOLIDATED BENCHMARK SYNTHESIS & CONCLUSION
    # =========================================================================
    story.append(Paragraph("6. Consolidated Multi-Case Synthesis & Verification", h1_style))
    story.append(Paragraph(
        "The following consolidated benchmark table summarizes empirical performance metrics across all four project variants under identical network conditions:",
        body_style
    ))

    # Consolidated Metrics Table
    summary_data = [
        [
            Paragraph("<b>Evaluation Case</b>", table_header),
            Paragraph("<b>Throughput (p=0.0)</b>", table_header),
            Paragraph("<b>Throughput (p=0.2)</b>", table_header),
            Paragraph("<b>Throughput (p=0.5)</b>", table_header),
            Paragraph("<b>Throughput (p=0.8)</b>", table_header),
            Paragraph("<b>Wire Overhead (p=0.8)</b>", table_header),
            Paragraph("<b>Speedup vs Case 1 (p=0.8)</b>", table_header)
        ],
        [
            Paragraph("<b>Part I (Single Client)</b>", table_cell),
            Paragraph("91.26 MB/s", table_cell_center),
            Paragraph("N/A", table_cell_center),
            Paragraph("N/A", table_cell_center),
            Paragraph("N/A", table_cell_center),
            Paragraph("1.000× (0% redundant)", table_cell_center),
            Paragraph("N/A", table_cell_center),
        ],
        [
            Paragraph("<b>Case 1 (Naive Restart)</b>", table_cell),
            Paragraph("86.72 MB/s", table_cell_center),
            Paragraph("69.36 MB/s", table_cell_center),
            Paragraph("53.70 MB/s", table_cell_center),
            Paragraph("19.26 MB/s", table_cell_center),
            Paragraph("3.001× (200% retransmitted)", table_cell_center),
            Paragraph("1.00× (baseline)", table_cell_center),
        ],
        [
            Paragraph("<b>Case 2 (Session Resume)</b>", table_cell),
            Paragraph("87.06 MB/s", table_cell_center),
            Paragraph("78.51 MB/s", table_cell_center),
            Paragraph("78.05 MB/s", table_cell_center),
            Paragraph("70.14 MB/s", table_cell_center),
            Paragraph("1.002× (&lt;0.2% redundant)", table_cell_center),
            Paragraph("<b>3.64× faster</b>", table_cell_center),
        ],
        [
            Paragraph("<b>Case 2 Enhanced (4× TCP)</b>", table_cell),
            Paragraph("<b>312.06 MB/s</b>", table_cell_center),
            Paragraph("<b>287.39 MB/s</b>", table_cell_center),
            Paragraph("<b>245.22 MB/s</b>", table_cell_center),
            Paragraph("<b>197.49 MB/s</b>", table_cell_center),
            Paragraph("1.028× (~2.8% redundant)", table_cell_center),
            Paragraph("<b>10.25× faster</b>", table_cell_center),
        ],
    ]

    t_summary = Table(summary_data, colWidths=[105, 68, 68, 68, 68, 77, 76])
    t_summary.setStyle(TableStyle([
        ('BACKGROUND', (0,0), (-1,0), colors.HexColor('#0D47A1')),
        ('GRID', (0,0), (-1,-1), 0.5, colors.HexColor('#B0BEC5')),
        ('VALIGN', (0,0), (-1,-1), 'MIDDLE'),
        ('ROWBACKGROUNDS', (0,1), (-1,-1), [colors.white, colors.HexColor('#F8F9FA')]),
        ('TOPPADDING', (0,0), (-1,-1), 4),
        ('BOTTOMPADDING', (0,0), (-1,-1), 4),
    ]))
    story.append(t_summary)
    story.append(Spacer(1, 8))

    story.append(Paragraph("<b>Cryptographic Verification & Quality Assurance:</b>", h2_style))
    story.append(Paragraph(
        "To rigorously confirm data integrity, the automated test harness executes <code>scripts/verify.sh</code> after every benchmark run. "
        "The verification script recursively hashes every downloaded file using <b>SHA-256</b> and compares digests directly against the original dataset. "
        "Across all fault probabilities (<i>p &isin; {0.0, 0.2, 0.5, 0.8}</i>) and all concurrency levels (up to <i>N=16</i>), <b>100% cryptographic integrity</b> "
        "was verified over all 1,860 files. Additionally, the verification confirmed zero corrupted files, zero truncated lengths, and zero leftover <code>.part</code> "
        "or <code>.ckpt</code> temporary artifacts.",
        body_style
    ))

    story.append(Paragraph("7. Conclusion & Architectural Key Takeaways", h1_style))
    story.append(Paragraph(
        "This project comprehensively implements and empirically evaluates a fault-tolerant topic-based file distribution service in C11. "
        "The experimental investigation provides several fundamental networking insights:",
        body_style
    ))
    story.append(Paragraph(
        "1. <b>Concurrency Serialization:</b> Single-threaded servers suffer immediate head-of-line blocking under concurrent traffic. Detached multi-threading "
        "delivers near-linear scalability (up to 466.8 MB/s at <i>N=16</i>) by overlapping socket I/O across CPU cores.",
        bullet_style
    ))
    story.append(Paragraph(
        "2. <b>The Geometric Cost of Stateless Restarts:</b> Under ungraceful TCP connection aborts, naive retry logic exhibits a geometric increase in required "
        "connection attempts (<i>E = 1 / (1-p)</i>), resulting in a 3.0× wire retransmission penalty and a 78% throughput collapse at <i>p=0.8</i>.",
        bullet_style
    ))
    story.append(Paragraph(
        "3. <b>Zero-Cost Fault Tolerance via Atomic Checkpointing:</b> Atomic client disk checkpoints combined with server-side high-water mark validation "
        "completely eliminate redundant retransmissions, maintaining &lt;0.2% wire overhead and sustaining 70.1 MB/s goodput under severe faults.",
        bullet_style
    ))
    story.append(Paragraph(
        "4. <b>Parallel Multiplexing & Fault Isolation:</b> Case 2 Enhanced proves that multi-socket pipelining with dynamic work queueing unlocks massive performance gains: "
        "achieving <b>312.1 MB/s at <i>p=0</i> (3.6× speedup)</b> and <b>197.5 MB/s at <i>p=0.8</i> (10.25× speedup over Case 1)</b> through seamless fault isolation.",
        bullet_style
    ))

    # Build Document
    doc.build(story, canvasmaker=NumberedCanvas)
    print(f"[+] Final Report PDF successfully generated: {pdf_path}")

if __name__ == "__main__":
    build_pdf()
