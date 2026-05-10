# **Advanced Audio Algorithms for Acoustic Echo Cancellation and Noise Reduction in 5G VoLTE and VoNR Applications**

The relentless progression of cellular network topologies from legacy circuit-switched architectures to fully packet-switched Internet Protocol (IP) networks has fundamentally altered the parameters of audio processing in mobile communications and the Internet of Things (IoT). As the global telecommunications infrastructure transitions from 4G Long Term Evolution (LTE) networks to 5G Standalone (SA) configurations, the establishment of native Voice over New Radio (VoNR) mandates highly efficient, ultra-low latency audio processing pipelines. In contemporary cellular devices, extracting intelligible near-end speech while concurrently suppressing background noise and cancelling acoustic echo constitutes a profoundly complex computational challenge.

This comprehensive report provides an exhaustive technical analysis of the state-of-the-art Acoustic Echo Cancellation (AEC) and Noise Reduction (NR) algorithms deployed in modern VoLTE and VoNR environments. The analysis examines the intersection of deep learning and traditional digital signal processing (DSP), the stringent network-level requirements imposed by 3GPP 5G specifications, and the specialized semiconductor intellectual property (IP) and software ecosystems provided by industry leaders such as Cadence Design Systems, CEVA, and NXP Semiconductors.

## **The Evolution of Cellular Voice Services and 5G Network Architecture**

Historically, mobile voice calls were managed by circuit-switched networks, where a dedicated communication channel was exclusively allocated for the duration of the conversation.1 The advent of 4G LTE introduced a data-centric paradigm, eventually leading to the deployment of Voice over LTE (VoLTE), the first solution to fully integrate voice transmissions as data packets over an IP network, eliminating the necessity for devices to fall back to legacy 2G or 3G infrastructures.1

The deployment of 5G SA networks continues this architectural evolution with Voice over New Radio (VoNR). VoNR operates entirely independently of legacy infrastructures, leveraging the 5G Radio Access Network (RAN) and the 5G Core (5GC) to deliver native voice services.1 The architecture acts as a sophisticated bridge: the 5G transport layer provides access, control, and packet delivery, while the IP Multimedia Subsystem (IMS) oversees the actual voice-session control, routing, and service logic.3

### **Core Network Functions and Call Flow Mechanics**

Within the 5G Core network, several critical network functions orchestrate the VoNR service. The Access and Mobility Management Function (AMF) handles connection and mobility management, while the Session Management Function (SMF) and User Plane Function (UPF) establish the routing paths for packet data.4 The Policy Control Function (PCF) ensures that the correct Quality of Service (QoS) parameters are applied, supported by the Unified Data Management (UDM) and Authentication Server Function (AUSF) for subscriber verification.4

The establishment of a VoNR call requires a highly choreographed sequence of signaling and bearer allocations. Initially, the User Equipment (UE) establishes a default Internet Protocol Data Unit (PDU) session pointing to the IMS Data Network Name (DNN).2 This initial session utilizes a non-Guaranteed Bit Rate (non-GBR) QoS flow mapped to a 5G QoS Identifier (5QI) of 5\.2 The 5QI=5 designation ensures that the Session Initiation Protocol (SIP) signaling messages utilized between the UE and the IMS are treated with high priority to guarantee reliable and low-latency call setup, even under network congestion.2

Upon successful SIP negotiation, a dedicated PDU session is established specifically for the transmission of the voice payload via the Real-Time Transport Protocol (RTP).2 This dedicated session is assigned a GBR QoS flow with a 5QI of 1, ensuring a strictly maintained bit rate, low latency, and low packet loss required for conversational audio.2 At the radio link level, the next-generation NodeB (gNB) utilizes the Radio Link Control Acknowledged Mode (RLC-AM) for the critical SIP signaling Data Radio Bearers (DRBs), while employing the Unacknowledged Mode (RLC-UM) for the RTP voice traffic DRBs to prevent retransmission delays from violating the strict real-time latency bounds.2 Furthermore, the Packet Data Convergence Protocol (PDCP) layer implements Robust Header Compression (RoHC) to minimize the overhead of IP/UDP/RTP headers, conserving precious air interface resources.2

### **Fallback Mechanisms and Network Emulation**

Because 5G SA coverage is not yet ubiquitous, the 3GPP standards define robust fallback mechanisms. When a UE attempts a voice call in an area where the 5G network cannot support the rigorous QoS requirements of VoNR, the network initiates an Evolved Packet System (EPS) Fallback.6 During EPS Fallback, the connection is handed down from the 5G NR to the legacy LTE network, routing the session through the Evolved Packet Core (EPC) to complete the call as a standard VoLTE transmission.7

The complexity of these interactions requires rigorous testing and validation architectures. Open-source initiatives like the OpenAirInterface Software Alliance (OSA) provide complete end-to-end implementations of the 5G SA protocol stack, offering Software-Defined Radio (SDR) based implementations of the gNodeB and 5G Core functions deployable via Docker containers.8 For commercial device validation, test equipment such as the Rohde & Schwarz CMX500 radio communication tester incorporates internal IMS servers to emulate virtual UEs, allowing engineers to test VoNR call setups, EPS fallback handovers, and execute audio quality measurements using algorithms like Perceptual Evaluation of Speech Quality (PESQ) and Perceptual Objective Listening Quality Analysis (POLQA).9

## **The Enhanced Voice Services (EVS) Codec and Audio Processing Constraints**

The 3GPP standards firmly position the Enhanced Voice Services (EVS) codec as the definitive audio coding standard for both VoLTE and VoNR.2 The transition to EVS represents a monumental leap in telecommunications audio fidelity, directly succeeding the Adaptive Multi-Rate Wideband (AMR-WB) codec previously standard in high-definition (HD) voice networks.1

While AMR-WB limits the acoustic capture to a 16 kHz sampling rate (an 8 kHz acoustic bandwidth), EVS introduces support for Super Wideband (SWB) operating at a 32 kHz sampling rate, and Fullband (FB) operating at a 48 kHz sampling rate.2 By supporting a 20 kHz acoustic bandwidth, EVS captures the entire spectrum of human hearing, delivering unprecedented clarity for conversational telephony, audiovisual conferencing, and mixed-content streaming.10

### **Processing Implications of Ultra-Wideband Audio**

The deployment of EVS SWB and FB profiles fundamentally alters the processing requirements placed upon the device's audio Digital Signal Processor (DSP). According to the Nyquist-Shannon sampling theorem, processing a 48 kHz Fullband signal requires the DSP to execute algorithms on 48,000 discrete audio samples per second per audio channel. For Acoustic Echo Cancellation (AEC), the adaptive filters must model the room impulse response over a sufficient time window. If an AEC algorithm requires a 100-millisecond filter tail to adequately cancel reverberant echo, an 8 kHz narrowband signal requires an 800-tap Finite Impulse Response (FIR) filter. Conversely, a 48 kHz Fullband signal requires a 4,800-tap FIR filter. Because the computational complexity of adaptive filtering scales with the number of taps, the transition to EVS exponentially increases the Millions of Instructions Per Second (MIPS) and memory bandwidth required from the hardware.

### **Network Impairments: Packet Loss and Jitter Buffer Management**

Real-world 5G networks are susceptible to RF interference, fading, and congestion, which manifest as packet loss and delay jitter. The EVS codec inherently addresses these impairments through a highly sophisticated, channel-aware framework featuring advanced Packet Loss Concealment (PLC) and an integrated Jitter Buffer Management (JBM) system.10 EVS PLC can synthesize up to four consecutive lost frames with minimal perceptible degradation to the user.15

However, the dynamic nature of the JBM introduces severe complications for the Acoustic Echo Cancellation pipeline.14 Delay jitter forces the receiving device to continuously adjust the depth of its jitter buffer, accelerating or decelerating the playback of the far-end audio signal through the local loudspeaker.14 Because the AEC algorithm relies on comparing the exact timing of the far-end reference signal against the microphone's captured signal, any dynamic alteration of the playback timing appears to the AEC as a sudden shift in the physical acoustic environment.13 If the DSP's AEC implementation is not tightly synchronized with the protocol stack's JBM module, the adaptive filters will continuously diverge, resulting in loud, unsuppressed echoes transmitted back to the far-end user.

| EVS Codec Feature | Technical Implication for Audio DSP Pipeline |
| :---- | :---- |
| **Super Wideband (32 kHz) & Fullband (48 kHz) Support** | Exponential increase in MIPS and memory required for FIR filter taps; dictates the necessity for wide-SIMD processing architectures. |
| **Adaptive Jitter Buffer Management (JBM)** | Requires instantaneous communication between the network stack and the DSP to prevent AEC adaptive filter divergence due to apparent acoustic path shifts. |
| **Packet Loss Concealment (PLC)** | Requires the DSP to dedicate memory and processing cycles to synthesize missing audio frames using predictive modeling prior to AEC processing. |
| **Backward Compatibility (AMR-WB)** | Demands flexible software frameworks capable of dynamically switching sampling rates and filter lengths mid-call without interrupting the audio stream. |

## **Algorithmic Foundations of Acoustic Echo Cancellation and Noise Reduction**

The primary objective of an AEC and NR system in a VoNR device is to extract clean, highly intelligible near-end speech while aggressively suppressing all other acoustic energy. In a full-duplex communication scenario, the device's microphone captures a composite signal consisting of the desired near-end speech, the ambient background noise, and the acoustic echo. The acoustic echo originates from the far-end user's voice being played through the local loudspeaker, propagating through the physical environment, bouncing off surfaces, and re-entering the local microphone.

Traditional AEC methodologies relied exclusively on linear adaptive filtering algorithms, predominantly variations of the Normalized Least Mean Squares (NLMS) or Recursive Least Squares (RLS) algorithms.16 These algorithms mathematically model the linear impulse response of the room and subtract a generated estimate of the echo from the microphone signal. However, modern smartphone and IoT form factors introduce severe non-linearities. Miniature loudspeakers driven at high volumes introduce harmonic distortion, amplifier clipping, and physical enclosure vibrations.16 Because linear adaptive filters cannot model non-linear distortions, a significant amount of residual echo remains in the signal.16

Furthermore, traditional noise reduction algorithms, such as Wiener filtering or spectral subtraction, excel at removing stationary background noises (e.g., HVAC hum, steady wind) by estimating the noise floor over time.18 However, these statistical methods fail catastrophically when confronted with highly non-stationary noises—such as keyboard clicking, barking dogs, passing sirens, or a vehicle hitting a speed bump—because the algorithms cannot adapt rapidly enough to the sudden spectral shifts, leading to artifacts, musical noise, and compromised speech intelligibility.17

### **The Paradigm Shift to Hybrid DSP-DNN Architectures**

To overcome the limitations of classical DSP, the industry has undergone a paradigm shift toward hybrid architectures that seamlessly fuse traditional adaptive filters with Deep Neural Networks (DNNs).16 Deploying pure, end-to-end deep learning models to handle the entirety of the AEC and NR workload requires massive parameter counts, rendering them unsuitable for real-time execution on low-resource embedded platforms.21

In state-of-the-art hybrid systems, the computational workload is strategically partitioned. A lightweight, classical adaptive filter is deployed to cancel the linear acoustic echo.16 Because adaptive filters are mathematically efficient at tracking dynamic room impulse responses, they handle the bulk of the echo suppression.16 The error signal from this adaptive filter—which contains the near-end speech, the background noise, and the non-linear residual echo—is then passed as an input feature to a compact DNN.16

The DNN is trained exclusively to act as a non-linear post-filter.16 Utilizing architectures such as Convolutional Recurrent Neural Networks (CRNNs), the DNN outputs a time-frequency spectral mask that maps the noisy input spectrogram to a clean speech spectrogram.17 For instance, the SOTA ULCNet model has been adapted for joint Acoustic Echo and Noise Reduction (AENR).20 By employing a single, ultra-low complexity neural network to simultaneously suppress both the non-linear residual echo and the ambient background noise, the hybrid ULCNet approach drastically lowers memory footprints and computational overhead, outperforming heavily parameter-laden networks while fitting within the strict power budgets of cellular IoT devices.20

### **Ultra-Low Latency Algorithmic Topologies**

While hybrid DNN architectures excel at noise suppression, their traditional reliance on the Short-Time Fourier Transform (STFT) introduces critical latency bottlenecks.24 STFT-based processing requires buffering audio samples into overlapping frames, intrinsically introducing algorithmic latencies between 16 and 32 milliseconds.24 While acceptable for standard telephony, this latency is highly detrimental for transparency modes in hearables, hearing aids, and specialized 5G Ultra-Reliable Low Latency Communication (URLLC) endpoints, where end-to-end delays exceeding 2 milliseconds induce a comb-filtering effect that disorients the user.24

Recent breakthroughs have demonstrated that sub-millisecond algorithmic latency is achievable by abandoning STFT masking in favor of DNN-driven time-domain filtering.24 By utilizing a lightweight Long Short-Term Memory (LSTM) network comprising approximately 626,000 parameters, SOTA systems can dynamically generate the tap weights for a minimum-phase Finite Impulse Response (FIR) filter.24 This approach enables pure sample-by-sample processing without frame buffering.24 Deployed on low-power DSP hardware, this LSTM-FIR architecture operates at a highly efficient 376 MIPS, achieving a mean algorithmic latency of just 0.32 to 1.25 milliseconds and a total end-to-end processing latency of 3.35 milliseconds.24

## **Software Architectures and Firmware Abstraction**

The execution of these advanced AENR algorithms requires sophisticated orchestration between the device's main Application Processor (AP) and the dedicated audio DSP. In heterogeneous System-on-Chip (SoC) architectures, the main processor typically runs a high-level operating system like Linux or Android, while the DSP executes a Real-Time Operating System (RTOS) such as Zephyr or FreeRTOS.25

Inter-processor communication is frequently managed by frameworks like OpenAMP.26 Because heterogeneous cores often maintain different physical memory maps, moving audio buffers between the Linux AP and the Zephyr RTOS DSP can introduce high latencies if the data must be constantly copied. To mitigate this, advanced abstraction layers employ generic address translation, utilizing mapping tables to resolve address mismatches and permit true zero-copy shared memory access.26

Furthermore, the industry is increasingly adopting the Sound Open Firmware (SOF) framework.27 SOF provides an open-source, modular firmware infrastructure that abstracts the specific complexities of the underlying DSP hardware.27 For example, MediaTek's Genio platform utilizes SOF to provide a unified software environment across its entire IoT SoC lineup. The Genio 1200 and Genio 350 leverage Cadence Tensilica HiFi 4 DSP cores, while the higher-tier Genio 700 and Genio 510 incorporate the more powerful HiFi 5 DSPs.27 SOF ensures that SOTA noise reduction algorithms can be seamlessly ported and executed across these varying silicon architectures without requiring total firmware rewrites.27

To accelerate the tuning and deployment of these algorithms, system integrators rely heavily on visual development environments such as DSP Concepts' Audio Weaver.28 Operating analogously to TensorFlow in the machine learning domain, Audio Weaver provides a modular drag-and-drop interface containing low-level IP blocks for beamforming, echo cancellation, and speaker equalization, allowing engineers to rapidly prototype and compile AENR pipelines directly to the target DSP architecture.28

| MediaTek Genio Platform | Integrated Audio DSP | Core Clock Speed | Target Application / Tier |
| :---- | :---- | :---- | :---- |
| **Genio 1200 (MT8395)** | Cadence Tensilica HiFi 4 | 200 MHz \- 720 MHz | Premium IoT, Edge AI |
| **Genio 700 (MT8390)** | Cadence Tensilica HiFi 5 | 260 MHz \- 800 MHz | Advanced Smart Home, High-End Audio |
| **Genio 510 (MT8370)** | Cadence Tensilica HiFi 5 | 260 MHz \- 800 MHz | Mid-Tier Smart Audio |
| **Genio 350 (MT8365)** | Cadence Tensilica HiFi 4 | Scaled appropriately | Entry-Level Voice Interfaces |

## **Deep Dive: Cadence Tensilica HiFi DSP Solutions**

Cadence Design Systems has firmly established the Tensilica HiFi DSP family as the preeminent architecture for Audio, Voice, and Speech (AVS) processing across the semiconductor industry. Licensed by over 70 companies and integrated into designs by more than 75 top-tier OEMs, the HiFi ecosystem supports over 175 production-ready software packages.29 The HiFi architecture is highly scalable; from the ultra-low energy HiFi Mini designed for always-listening voice triggers, to the HiFi 3z optimized specifically for EVS codec acceleration, up to the highly parallelized HiFi 4 and HiFi 5s processors.30

### **The Sixth-Generation Tensilica HiFi iQ Architecture**

To address the exponential computational requirements of AI-enhanced audio, spatial rendering, and Fullband EVS processing, Cadence introduced the sixth-generation Tensilica HiFi iQ DSP.32 Built upon the new Xtensa LX8 platform, the HiFi iQ represents a foundational architectural overhaul designed explicitly for Voice AI.34

A critical evolution in the HiFi iQ is the expansion of the vector processing unit. Moving from the 128-bit architecture of the HiFi 5s to a massive 256-bit Single Instruction, Multiple Data (SIMD) pipeline, the HiFi iQ allows for profound parallelization of mathematical operations.34 The processor features 5 Very Long Instruction Word (VLIW) slots, dual memory load units, and an expansive 80-bit Multiply-Accumulate (MAC) accumulator width to maintain extreme precision during complex filter calculations.31

The integration of native AI acceleration is a primary differentiator. The HiFi iQ supports advanced vector floating-point formats, natively executing FP8 and BF16 instructions, which are critical for deploying heavily quantized neural networks, alongside traditional FP16, FP32, and FP64 precision.31 The inclusion of dedicated AI MAC configurations (capable of executing 16x8, 16x4, 8x8, and 8x4 operations) results in a staggering 8X increase in AI inference throughput compared to the previous HiFi 5s generation.31

This architectural density allows the HiFi iQ to execute not only hybrid AENR models but also full On-Device AI inference.36 The DSP possesses the computational capacity to run Small Language Models (SLMs) and even lightweight Large Language Models (LLMs) locally, enabling complex natural language processing without offloading data to the cloud, thereby eliminating network latency and securing user privacy.34

The generational performance metrics are definitive:

* **Raw Compute:** 2X raw compute capability versus the HiFi 5s.32  
* **AI Performance:** 8X higher AI throughput.32  
* **Energy Efficiency:** Over 25% energy savings across general workloads.32  
* **Codec Execution:** Greater than 40% performance uplift on complex codecs (e.g., EVS, Dolby MS12).33

Furthermore, Cadence provides extensive customization through the Tensilica Instruction Extension (TIE) language, allowing SoC designers to add custom, application-specific instructions directly into the DSP pipeline.31 For maximum throughput, the HiFi iQ can be paired symmetrically with Cadence's Neo Neural Processing Units (NPUs).34

### **Retune DSP Ecosystem Integration**

The hardware capabilities of the Tensilica DSPs are fully realized through close partnerships with software IP providers such as Retune DSP. Retune has heavily optimized its VoiceSpot and VoiceSeeker algorithms specifically for the Tensilica instruction set.34

VoiceSpot operates as a machine learning-based wake-word engine engineered with an ultra-small memory footprint, allowing it to reside perpetually in the DSP's tightly coupled memory (TCM).38 Upon keyword detection, VoiceSpot triggers Retune’s multi-microphone VoiceSeeker technology, which executes adaptive near-field and far-field beamforming alongside robust acoustic echo cancellation.38 By optimizing the code directly for the DSP's dual load/store architecture and VLIW capabilities, Retune maximizes the MIPS efficiency of the system, preserving critical battery life in cellular endpoints.39

## **Deep Dive: CEVA Intelligent Edge Audio Processing**

CEVA commands a massive presence in the intellectual property market for edge DSPs, providing highly refined architectures explicitly engineered for sensor fusion, computer vision, and audio AI. CEVA's deployment strategy leverages highly specialized hardware configurations—specifically the SensPro2 family—paired with proprietary, rigorously optimized software suites like ClearVox.

### **SensPro2 Hardware Architecture**

The SensPro2 architecture operates as a highly configurable sensor hub that fuses the classical DSP capabilities of CEVA's BX2 processors with the advanced matrix math execution of their NeuPro Deep Learning Accelerators (DLAs).40 When compared to the first-generation SensPro hardware manufactured on identical process nodes, SensPro2 delivers twice the AI inference throughput and twice the memory bandwidth, concurrently achieving a 20% reduction in power consumption.40

To address the specific constraints of audio AI, acoustic sensing, and Natural Language Processing (NLP) without over-provisioning silicon area, CEVA introduced specialized SKUs within the SensPro2 lineup: the SP50 and SP100 configurations.40 These variants are equipped with smaller INT8 MAC arrays—integrating 64 and 128 INT8 MAC units, respectively.40 This targeted reduction in MAC density dramatically decreases the die area and power consumption while providing ample processing headroom for voice enhancement algorithms.40 The architecture is highly scalable; each INT8 MAC array can execute optimized scaling operations, performing one-fourth of the INT16 MAC operations or one-sixteenth of the INT32 MAC operations per cycle.40

For applications requiring extreme mathematical precision, such as automotive systems demanding ASIL B fault diagnostics and ASIL D functional safety, CEVA offers floating-point variants (the SPF2 and SPF4) that integrate single-precision and half-precision MAC units while omitting integer execution.40 Translating neural networks from high-level frameworks (like TensorFlow Lite Micro) down to the bare-metal hardware is managed by the CEVA Deep Neural Network (CDNN) compiler.41 The CDNN compiler natively supports over 200 neural network topologies, applying automated graph optimization, layer-specific scaling, and dynamic retraining to ensure the models execute at peak efficiency on the SensPro2 silicon.41

### **ClearVox ENC: Neural-Network Environmental Noise Cancellation**

On the software layer, CEVA provides the ClearVox Audio Front-End (AFE), an embedded software suite that merges classical signal processing for Direction of Arrival (DOA) estimation with cutting-edge neural networks.42

The zenith of this suite is the ClearVox ENC (Environmental Noise Cancellation) algorithm. Explicitly designed to combat highly challenging ambient noise scenarios, ClearVox ENC utilizes a trained, highly compact neural network operating entirely on-device, completely circumventing the need for cloud-AI offloading.44 This localized execution is critical for eliminating Internet latency bottlenecks and addressing data privacy mandates.44

A defining characteristic of ClearVox ENC is its ability to operate utilizing only a single local microphone input, significantly reducing hardware BOM (Bill of Materials) costs for device manufacturers.44 Furthermore, unlike conventional noise reduction algorithms that only filter the outbound transmission, ClearVox ENC features unique bidirectional processing.44 The algorithm simultaneously processes both the outgoing captured speech and the incoming received speech signal.44 By executing on both streams, it actively isolates and suppresses both continuous stationary noise (e.g., fans, road noise) and transient non-stationary noise (e.g., background conversations, sudden impacts) for both parties engaged in the VoNR call.44

The ClearVox ENC software processes audio at a 16 kHz sampling rate with 16-bit resolution, ensuring high speech fidelity and the capability to cancel broad-spectrum and high-frequency environmental noise.44 Through meticulous code optimization, the algorithm maintains such a diminutive memory and MIPS footprint that it can run co-residentially with other intensive applications, such as the Ceva-RealSpace spatial audio engine, on highly constrained ARM MCUs or Ceva-BX DSP cores.44

| CEVA Technology | Primary Function | Technical Differentiators |
| :---- | :---- | :---- |
| **SensPro2 SP50/SP100** | Audio AI DSP Hardware | 64/128 INT8 MAC arrays, 2X inference throughput vs Gen 1, optimized for low die area and power. |
| **CDNN Compiler** | Neural Network Translation | Supports \>200 network topologies, automated graph optimization and layer scaling for TensorFlow Lite. |
| **ClearVox ENC** | Environmental Noise Cancellation | Single-microphone input, bidirectional processing (inbound/outbound), pure edge execution, low MIPS footprint. |

## **Deep Dive: NXP Semiconductors Voice and Automotive Solutions**

NXP Semiconductors delivers an expansive portfolio of voice processing technologies targeted at the mobile, smart home, and automotive sectors. NXP’s strategy combines high-performance i.MX applications processors with highly tuned, proprietary software libraries. A significant portion of NXP’s audio innovation is centralized at their Smart Home Innovation Lab in Austin, Texas, a multi-million-dollar R\&D facility outfitted with state-of-the-art anechoic chambers, Head and Torso Simulators (HATS), Sound Pressure Level (SPL) meters, and automated testing rigs crucial for precise algorithm calibration and Amazon Alexa Voice Service (AVS) certification.45

### **The VoiceSeeker and VoiceSpot Framework**

The foundation of NXP’s embedded voice suite is managed by a software Audio Front End (AFE) router, a dynamic program that directs raw PCM audio buffers to designated processing engines at runtime.47

VoiceSeeker (specifically, VoiceSeeker Light) serves as NXP's core voice-processing library.47 VoiceSeeker is tasked with multi-microphone beamforming, Acoustic Echo Cancellation, and resolving acoustic delays.47 A distinct engineering advantage of VoiceSeeker is its extreme flexibility regarding hardware design; the algorithm does not require a predetermined, fixed microphone geometry.47 It dynamically adapts to linear, triangular, or circular arrays with variable microphone spacing (ideally 4 cm, but functioning across a 2 cm to 8 cm range) and imposes no strict tolerance for microphone matching.47

Profiling data for VoiceSeeker executing on an ARM Cortex-M7 core illustrates its high efficiency:

* **Latency Profile:** The algorithm introduces merely 12 ms of latency without AEC, and 14 ms when AEC is active.48  
* **Resource Scaling:** A bare-minimum two-microphone implementation without AEC consumes approximately 14 MHz and 60 kB of memory.48 A robust deployment utilizing three microphones, two loudspeakers, and active AEC scales predictably to 320 MHz and 290 kB of memory.48

Once VoiceSeeker cleans the audio stream, it passes the buffer directly to VoiceSpot, a compact, low-vocabulary speech detection engine designed specifically for wake-word triggering and localized command detection.47

### **Conversa Voice Suite for Cellular Telephony**

For the rigorous, full-duplex demands of VoLTE and VoNR cellular communication, NXP deploys the Conversa Voice Suite.19 Conversa provides absolute control over both the uplink and downlink audio paths, engineering the signal to meet the exacting specifications required for Microsoft Teams certification.51

A cornerstone of the Conversa suite is its Machine Learning Noise Reduction (MLNR) technology.19 While standard noise reduction techniques fail against transient, non-stationary noises, the Conversa MLNR models are trained to instantly recognize and subtract disruptive acoustic anomalies—such as keyboard typing, sirens, or barking dogs—from the user's speech path without introducing robotic artifacts or severe voice distortion.19

### **SAF9100 Automotive DSP and BdSound Integration**

The modern Software-Defined Vehicle (SDV) requires an audio architecture exponentially more complex than traditional mobile devices, demanding the simultaneous execution of AEC, road noise cancellation, and spatial rendering. To address this, NXP developed the SAF9100, a massive automotive audio one-chip solution.52 The SAF9100 SoC integrates an embedded MCU, PCIe and Gigabit Ethernet connectivity, hardware biquad accelerators, ASIL-A certified audio pathways, and is anchored by dual Cadence Tensilica HiFi 5 DSP cores enhanced with dedicated AI/ML accelerators.52

To harness the vast computational power of the SAF9100, NXP forged a strategic partnership with BdSound to integrate the *Simply Sounds Clear* technology suite.25 The BdSound software stack is meticulously designed to counteract the unique acoustic chaos of the automotive cabin. When traditional DSP algorithms encounter the sudden, chaotic noise pattern of a vehicle accelerating over a speed bump, the filters struggle to adapt, leaking substantial residual noise into the communication channel.18 BdSound leverages highly trained AI models to identify and aggressively suppress these severe non-stationary structural noises instantly.18

Furthermore, the *Simply Sounds Clear* suite enables advanced "Microphone Bubbles"—a sophisticated spatial isolation technique that utilizes localized microphones to project a virtual acoustic barrier around a specific passenger.25 This allows the system to capture only the designated speaker's voice while entirely muting adjacent passengers or external cabin noise.25 The BdSound AEC algorithms operate at sample frequencies up to 48 kHz, perfectly aligning with the 3GPP EVS Fullband specifications to ensure flawless, high-fidelity teleconferencing from within the vehicle.25

| NXP Software Framework | Core Capability | Notable Features / Metrics | Target Platform |
| :---- | :---- | :---- | :---- |
| **VoiceSeeker** | Audio Front End (AFE) | Geometry-agnostic beamforming, 12-14ms latency, dynamic scaling (14MHz to 320MHz). | i.MX Series, Cortex-M, IoT Edge |
| **VoiceSpot** | Wake Word Engine | Operates sequentially post-VoiceSeeker, small footprint. | Always-on listening devices |
| **Conversa Voice Suite** | Full-Duplex Telephony | Machine Learning Noise Reduction (MLNR), MS Teams certification. | Mobile Handsets, VoLTE/VoNR |
| **Simply Sounds Clear (BdSound)** | Automotive Cabin Audio | "Microphone Bubbles", 48 kHz AEC, AI-based non-stationary road noise suppression. | SAF9100 Audio DSP, In-Car Comms |

## **Power Management Constraints in Cellular IoT Implementations**

The massive deployment of Cellular IoT (cIoT) and Machine-Type Communication (mMTC) across 5G networks introduces extreme constraints on audio DSP pipelines. Unlike smartphones with relatively large batteries, IoT endpoints—such as remote industrial sensors or smart city infrastructure—must operate on highly restricted power budgets, often requiring multi-year lifespans from a single battery cell.56

To facilitate these power requirements, 5G NR specifications allow IoT devices to heavily utilize Discontinuous Reception (DRX) and extended DRX (eDRX) cycles at the MAC layer.2 During these cycles, the radio transceiver and the primary Application Processor enter deep sleep modes, avoiding Physical Downlink Control Channel (PDCCH) monitoring and Synchronization Signal Block (SSB) processing entirely unless explicitly paged.7

Consequently, the audio subsystem must operate with microscopic duty cycles. A typical IoT audio architecture relies on heavily cascaded activation states. Initially, an ultra-low-power analog or basic digital Voice Activity Detector (VAD) monitors the environment in the microwatt range.43 Only upon the detection of vocal energy is a lightweight wake-word engine (such as Retune VoiceSpot running on a heavily down-clocked HiFi Mini or CEVA SP50 core) activated.38 If the specific activation intent is verified, the system wakes the primary AP and instantiates the complex, hybrid DNN-AEC algorithms to process the EVS-encoded GBR payload for the duration of the VoNR SIP transmission.2 This cascading architecture ensures that the mathematically intense, high-MIPS Deep Learning networks are strictly gated, preventing rapid battery depletion while maintaining immediate responsiveness when user interaction is required.

## **Future Outlook**

The landscape of Acoustic Echo Cancellation and Noise Reduction in VoLTE and VoNR environments is undergoing a profound and rapid architectural metamorphosis. The industry is witnessing a definitive transition away from purely linear mathematical signal processing models toward deeply integrated hybrid systems and highly quantized, low-parameter neural networks.

The integration of dedicated AI Multiply-Accumulate (MAC) units directly into the silicon of audio DSPs—exemplified by the massive 8X AI throughput capability of the Cadence HiFi iQ and the specialized INT8 matrices within CEVA's SensPro2—demonstrates that edge inference is no longer an optional secondary co-processor, but the foundational architecture of modern acoustic signal processing. Furthermore, the universal adoption of the 3GPP EVS codec for 5G voice services forces all AEC algorithms to operate seamlessly at sampling rates up to 48 kHz. This vast spectral expansion demands computational throughputs that can only be satisfied by highly parallelized, ultra-wide SIMD hardware architectures.

As 5G Standalone networks continue to mature and the 5G Core comprehensively replaces legacy EPS infrastructures, the requirement for flawless, full-duplex, cloud-independent voice processing will only intensify. Semiconductor designers and software engineers will continue to deeply intertwine artificial intelligence with traditional DSP, relentlessly driving down latency profiles below the one-millisecond threshold, minimizing power consumption, and redefining the boundaries of acoustic clarity in the wireless domain.

#### **Works cited**

1. VoLTE and VoNR: The Evolution of Voice in Mobile Networks \- Telit Cinterion, accessed May 7, 2026, [https://www.telit.com/blog/volte-and-vonr-voice-in-mobile-networks/](https://www.telit.com/blog/volte-and-vonr-voice-in-mobile-networks/)  
2. Voice Over NR | VoNR Call Flow \- Voice Services \- Techplayon, accessed May 7, 2026, [https://www.techplayon.com/voice-over-nr-vonr-call-flow/](https://www.techplayon.com/voice-over-nr-vonr-call-flow/)  
3. VoNR Explained | 5G Voice Architecture, IMS, VoNR Call Flow \- 3GLTEInfo, accessed May 7, 2026, [https://www.3glteinfo.com/5g/architecture/vonr/](https://www.3glteinfo.com/5g/architecture/vonr/)  
4. What is Voice over New Radio (VoNR), accessed May 7, 2026, [https://www.ng-voice.com/learning-center/what-is-voice-over-new-radio-vonr](https://www.ng-voice.com/learning-center/what-is-voice-over-new-radio-vonr)  
5. VOICE OVER NR, accessed May 7, 2026, [https://www.rohde-schwarz.taipei/data/activity/file/1644475344601810383.pdf](https://www.rohde-schwarz.taipei/data/activity/file/1644475344601810383.pdf)  
6. The Future of Voice in Mobile Wireless Communications \- 1 5G Americas, accessed May 7, 2026, [https://www.5gamericas.org/wp-content/uploads/2021/02/InDesign-Future-of-Voice-Feb-2021-1.pdf](https://www.5gamericas.org/wp-content/uploads/2021/02/InDesign-Future-of-Voice-Feb-2021-1.pdf)  
7. 5G NR Voice Solutions Overview and Deployment Guidelines \- MediaTek, accessed May 7, 2026, [https://i.mediatek.com/hubfs/MediaTek-5G-Voice-Solutions-Whitepaper-PDF5GNRSWP-0821.pdf?hsLang=en](https://i.mediatek.com/hubfs/MediaTek-5G-Voice-Solutions-Whitepaper-PDF5GNRSWP-0821.pdf?hsLang=en)  
8. 4G/5G Networks Software – Colosseum: The Open RAN Digital Twin, accessed May 7, 2026, [https://colosseum.sites.northeastern.edu/4g-5g-networks-software/](https://colosseum.sites.northeastern.edu/4g-5g-networks-software/)  
9. Testing voice services in 5G NR (VoNR) \- Rohde & Schwarz, accessed May 7, 2026, [https://www.rohde-schwarz.com/us/applications/testing-voice-services-in-5g-nr-vonr\_56279-1092032.html](https://www.rohde-schwarz.com/us/applications/testing-voice-services-in-5g-nr-vonr_56279-1092032.html)  
10. Enhanced Voice Services \- Wikipedia, accessed May 7, 2026, [https://en.wikipedia.org/wiki/Enhanced\_Voice\_Services](https://en.wikipedia.org/wiki/Enhanced_Voice_Services)  
11. 3GPP Enhanced Voice Services (EVS) codec \- Nokia, accessed May 7, 2026, [https://www.nokia.com/asset/f/200002/](https://www.nokia.com/asset/f/200002/)  
12. TR 126 952 \- V12.4.0 \- Universal Mobile Telecommunications System (UMTS); LTE; Codec for Enhanced Voice Services (EVS) \- ETSI, accessed May 7, 2026, [https://www.etsi.org/deliver/etsi\_tr/126900\_126999/126952/12.04.00\_60/tr\_126952v120400p.pdf](https://www.etsi.org/deliver/etsi_tr/126900_126999/126952/12.04.00_60/tr_126952v120400p.pdf)  
13. ETSI TS 126 448 V19.0.0 (2025-10), accessed May 7, 2026, [https://www.etsi.org/deliver/etsi\_TS/126400\_126499/126448/19.00.00\_60/ts\_126448v190000p.pdf](https://www.etsi.org/deliver/etsi_TS/126400_126499/126448/19.00.00_60/ts_126448v190000p.pdf)  
14. Packet Loss, Jitter, Delay and the New EVS Audio Codec \- Spirent, accessed May 7, 2026, [https://www.spirent.jp/blogs/packet-loss-jitter-delay-new-evs-codec](https://www.spirent.jp/blogs/packet-loss-jitter-delay-new-evs-codec)  
15. EVS Codec \- Enhanced Voice Services for HD Audio \- Consilient Technologies, accessed May 7, 2026, [https://www.consilient-tech.com/evs-enhanced-voice-services-codec](https://www.consilient-tech.com/evs-enhanced-voice-services-codec)  
16. Align-ULCNet: Towards Low-Complexity and Robust Acoustic Echo and Noise Reduction \- arXiv, accessed May 7, 2026, [https://arxiv.org/html/2410.13620v1](https://arxiv.org/html/2410.13620v1)  
17. Noise Cancellation in Voice Bot Audio Pre-Processing \- SIP Trunk \- ClearlyIP, accessed May 7, 2026, [https://go.clearlyip.com/articles/voice-audio-preprocessing-noise-cancellation](https://go.clearlyip.com/articles/voice-audio-preprocessing-noise-cancellation)  
18. How to make a world without noise: episode 3 \- BdSound, accessed May 7, 2026, [https://www.bdsound.com/how-to-make-a-world-without-noise-episode-3/](https://www.bdsound.com/how-to-make-a-world-without-noise-episode-3/)  
19. Conversa Voice Suite, accessed May 7, 2026, [https://www.nxp.com/assets/block-diagram/en/CONVERSA-VOICE-SUITE.pdf](https://www.nxp.com/assets/block-diagram/en/CONVERSA-VOICE-SUITE.pdf)  
20. A Hybrid Approach for Low-Complexity Joint Acoustic Echo and Noise Reduction \- arXiv, accessed May 7, 2026, [https://arxiv.org/html/2408.15746v1](https://arxiv.org/html/2408.15746v1)  
21. arXiv:2408.15746v1 \[eess.AS\] 28 Aug 2024, accessed May 7, 2026, [https://arxiv.org/pdf/2408.15746](https://arxiv.org/pdf/2408.15746)  
22. A Hybrid Approach for Low-Complexity Joint Acoustic Echo and Noise Reduction \- arXiv, accessed May 7, 2026, [https://arxiv.org/abs/2408.15746](https://arxiv.org/abs/2408.15746)  
23. Cadence Tensilica Edge AI Processor IP Solutions for Broad Market Use Cases, accessed May 7, 2026, [https://www.edge-ai-vision.com/wp-content/uploads/2020/11/Desai\_Cadence\_2020\_Embedded\_Vision\_Summit\_Slides\_Final.pdf](https://www.edge-ai-vision.com/wp-content/uploads/2020/11/Desai_Cadence_2020_Embedded_Vision_Summit_Slides_Final.pdf)  
24. Towards Sub-millisecond Latency Real-Time Speech Enhancement Models on Hearables, accessed May 7, 2026, [https://arxiv.org/html/2409.18239v2](https://arxiv.org/html/2409.18239v2)  
25. Simply Sounds Clear™ \- BdSound: Top-Notch Speech Quality ..., accessed May 7, 2026, [https://www.bdsound.com/simplysoundsclear/](https://www.bdsound.com/simplysoundsclear/)  
26. Running Zephyr RTOS on Cadence Tensilica HiFi 4 DSP \- NXP Semiconductors, accessed May 7, 2026, [https://www.nxp.com/docs/en/application-note/AN13970.pdf](https://www.nxp.com/docs/en/application-note/AN13970.pdf)  
27. Unlocking Advanced Audio Processing: Leveraging HiFi DSP and SOF on the MediaTek Genio Platform, accessed May 7, 2026, [https://genio.mediatek.com/blog/unlocking-advanced-audio-processing-leveraging-hifi-dsp-and-sof-on-the-mediatek-genio-platform](https://genio.mediatek.com/blog/unlocking-advanced-audio-processing-leveraging-hifi-dsp-and-sof-on-the-mediatek-genio-platform)  
28. DSP Concepts Enables Audio Weaver for the Cadence Tensilica HiFi 5 DSP \- EDN, accessed May 7, 2026, [https://www.edn.com/dsp-concepts-enables-audio-weaver-for-the-cadence-tensilica-hifi-5-dsp/](https://www.edn.com/dsp-concepts-enables-audio-weaver-for-the-cadence-tensilica-hifi-5-dsp/)  
29. Audio software framework for DSP-based SoCs \- EE World Online, accessed May 7, 2026, [https://www.eeworldonline.com/audio-software-framework-dsp-based-socs/](https://www.eeworldonline.com/audio-software-framework-dsp-based-socs/)  
30. Cadence Tensilica HiFi DSP, accessed May 7, 2026, [https://site.eet-china.com/webinar/pdf/Cadence\_TIP\_PB\_HiFi\_DSP\_FINAL\_datasheet01.pdf](https://site.eet-china.com/webinar/pdf/Cadence_TIP_PB_HiFi_DSP_FINAL_datasheet01.pdf)  
31. HiFi iQ DSP | Cadence, accessed May 7, 2026, [https://www.cadence.com/en\_US/home/tools/silicon-solutions/compute-ip/hifi-dsps/hifi-iq.html](https://www.cadence.com/en_US/home/tools/silicon-solutions/compute-ip/hifi-dsps/hifi-iq.html)  
32. Listen up: Cadence HiFi iQ DSP amps audio, voice AI processing \- Fierce Sensors, accessed May 7, 2026, [https://www.fiercesensors.com/ai/listen-cadence-hifi-iq-dsp-amps-audio-voice-ai-processing](https://www.fiercesensors.com/ai/listen-cadence-hifi-iq-dsp-amps-audio-voice-ai-processing)  
33. Cadence launches Tensilica HiFi iQ DSP for voice AI SoCs \- ENGtechnica, accessed May 7, 2026, [https://engtechnica.com/cadence-launches-tensilica-hifi-iq-dsp-for-voice-ai-socs/](https://engtechnica.com/cadence-launches-tensilica-hifi-iq-dsp-for-voice-ai-socs/)  
34. Cadence Unwraps DSP IP Purpose-Built for Next-Gen Voice AI and Audio \- News, accessed May 7, 2026, [https://www.allaboutcircuits.com/news/cadence-unwraps-dsp-ip-purpose-built-for-next-gen-voice-ai-and-audio/](https://www.allaboutcircuits.com/news/cadence-unwraps-dsp-ip-purpose-built-for-next-gen-voice-ai-and-audio/)  
35. Cadence Unveils Its Sixth Generation Tensilica Hi-Fi iQ DSP | TechPowerUp, accessed May 7, 2026, [https://www.techpowerup.com/345456/cadence-unveils-its-sixth-generation-tensilica-hi-fi-iq-dsp](https://www.techpowerup.com/345456/cadence-unveils-its-sixth-generation-tensilica-hi-fi-iq-dsp)  
36. Tensilica HiFi iQ DSP | On-Device AI, LLM Support, and Immersive Audio \- YouTube, accessed May 7, 2026, [https://www.youtube.com/watch?v=8EieHmfAkQ4](https://www.youtube.com/watch?v=8EieHmfAkQ4)  
37. Cadence Unveils HiFi iQ DSP to Power Next-Gen Voice AI and Audio \- TradingView, accessed May 7, 2026, [https://www.tradingview.com/news/zacks:1b6cd90b8094b:0-cadence-unveils-hifi-iq-dsp-to-power-next-gen-voice-ai-and-audio/](https://www.tradingview.com/news/zacks:1b6cd90b8094b:0-cadence-unveils-hifi-iq-dsp-to-power-next-gen-voice-ai-and-audio/)  
38. Expanding 'The Voice of Things' \- NXP Adds Retune DSP to Its Broad Portfolio of Voice Enablement Solutions, accessed May 7, 2026, [https://www.nxp.com/company/about-nxp/smarter-world-blog/BL-EXPANDING-THE-VOICE-OF-THINGS](https://www.nxp.com/company/about-nxp/smarter-world-blog/BL-EXPANDING-THE-VOICE-OF-THINGS)  
39. Echo cancellation technology ported to HiFi audio DSPs \- Electronic Specifier, accessed May 7, 2026, [https://www.electronicspecifier.com/products/design-automation/echo-cancellation-technology-ported-to-hifi-audio-dsps/](https://www.electronicspecifier.com/products/design-automation/echo-cancellation-technology-ported-to-hifi-audio-dsps/)  
40. Ceva SensPro2 Doubles AI Throughput, accessed May 7, 2026, [https://www.ceva-ip.com/wp-content/uploads/Ceva-SensPro2-Doubles-AI-Throughput.pdf](https://www.ceva-ip.com/wp-content/uploads/Ceva-SensPro2-Doubles-AI-Throughput.pdf)  
41. SensPro2™ \- Second Generation High Performance Sensor Hub DSP Architecture \- Ceva's IP, accessed May 7, 2026, [https://www.ceva-ip.com/wp-content/uploads/CEVA-SensPro-Intro.pdf](https://www.ceva-ip.com/wp-content/uploads/CEVA-SensPro-Intro.pdf)  
42. Ceva \- Arm, accessed May 7, 2026, [https://www.arm.com/partners/catalog/ceva](https://www.arm.com/partners/catalog/ceva)  
43. CEVA ClearVox(tm) provides voice processing software package that includes multi-mic beamforming, speaker Direction of Arrival, Noise suppression and Echo cancellation., accessed May 7, 2026, [https://armkeil.blob.core.windows.net/developer/Files/pdf/ai-ecosystem-catalogue/ceva-ai-partner-catalogue-clearvox-afe-software-suite.pdf](https://armkeil.blob.core.windows.net/developer/Files/pdf/ai-ecosystem-catalogue/ceva-ai-partner-catalogue-clearvox-afe-software-suite.pdf)  
44. Ceva ClearVox Advanced Voice Clarity with Noise Reduction, accessed May 7, 2026, [https://www.ceva-ip.com/product/ceva-clearvox/](https://www.ceva-ip.com/product/ceva-clearvox/)  
45. NXP's New Smart Home Innovation Lab, accessed May 7, 2026, [https://www.nxp.com/company/about-nxp/smarter-world-blog/BL-SMART-HOME-INNOVATION-LAB](https://www.nxp.com/company/about-nxp/smarter-world-blog/BL-SMART-HOME-INNOVATION-LAB)  
46. Audio Lab Services | NXP Semiconductors, accessed May 7, 2026, [https://www.nxp.com/support/support/nxp-engineering-services/professional-support-for-amazon-self-test-services-of-alexa-built-in-products-and-audio-lab-services:AUDIO-LAB-SERVICES](https://www.nxp.com/support/support/nxp-engineering-services/professional-support-for-amazon-self-test-services-of-alexa-built-in-products-and-audio-lab-services:AUDIO-LAB-SERVICES)  
47. AN14276 \- NXP Semiconductors, accessed May 7, 2026, [https://www.nxp.com/docs/en/application-note/AN14276.pdf](https://www.nxp.com/docs/en/application-note/AN14276.pdf)  
48. VoiceSeeker FAQ \- NXP Community, accessed May 7, 2026, [https://community.nxp.com/t5/Voice-software-technology/VoiceSeeker-FAQ/ta-p/1439912](https://community.nxp.com/t5/Voice-software-technology/VoiceSeeker-FAQ/ta-p/1439912)  
49. nxp-imx/imx-voiceui: voiceseeker voicespot vit libraries, voiceseeker wrapper and voiceUI unitest. \- GitHub, accessed May 7, 2026, [https://github.com/nxp-imx/imx-voiceui](https://github.com/nxp-imx/imx-voiceui)  
50. Voice Processing \- NXP Semiconductors, accessed May 7, 2026, [https://www.nxp.com/applications/technologies/human-machine-interface/voice-processing:VOICE](https://www.nxp.com/applications/technologies/human-machine-interface/voice-processing:VOICE)  
51. Clear Voice Calling Made Easys | NXP Semiconductors, accessed May 7, 2026, [https://www.nxp.com/design/design-center/software/embedded-software/application-software-packs/application-software-pack-conversa-voice-calling:APP-SW-PACK-CONVERSA-VOICE](https://www.nxp.com/design/design-center/software/embedded-software/application-software-packs/application-software-pack-conversa-voice-calling:APP-SW-PACK-CONVERSA-VOICE)  
52. One-Chip Solution, Scalable Audio DSP Processing with AI/ML Capability, accessed May 7, 2026, [https://www.nxp.com/assets/block-diagram/en/SAF9100.pdf](https://www.nxp.com/assets/block-diagram/en/SAF9100.pdf)  
53. Audio Processors \- NXP Semiconductors, accessed May 7, 2026, [https://www.nxp.com/products/audio/audio-processors:AUDIO-PROCESSORS](https://www.nxp.com/products/audio/audio-processors:AUDIO-PROCESSORS)  
54. Goodix and NXP Announce Collaboration in the Automotive Audio Market, accessed May 7, 2026, [https://www.goodix.com/en/about\_goodix/newsroom/company\_news/detail/4883](https://www.goodix.com/en/about_goodix/newsroom/company_news/detail/4883)  
55. THD 125 BD Sound In Car Voice Isolation Bubbles Built in AI Software to Enhance Voice Communication \- YouTube, accessed May 7, 2026, [https://www.youtube.com/watch?v=sHnfPImHmTw](https://www.youtube.com/watch?v=sHnfPImHmTw)  
56. State-of-the-Art and New Challenges in 5G Networks with Blockchain Technology \- MDPI, accessed May 7, 2026, [https://www.mdpi.com/2079-9292/13/5/974](https://www.mdpi.com/2079-9292/13/5/974)  
57. Cellular Internet of Things (IoT) in the 5G era \- Ericsson, accessed May 7, 2026, [https://www.ericsson.com/en/reports-and-papers/white-papers/cellular-iot-in-the-5g-era](https://www.ericsson.com/en/reports-and-papers/white-papers/cellular-iot-in-the-5g-era)