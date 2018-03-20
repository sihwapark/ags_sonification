// MAT 240B Final Project
// Behavioral Data Sonfication based on Asynchronous Granular Synthesis

// This project is to make an interface for personal data sonification 
// based on Asynchronous Granular Synthesis (AGS). 

// The data is about my phone use behavior and includes the 11,367 records of a location, date, duration of 
// each phone use since January 20, 2017. For this interface, it only uses hourly use duration.

// For implementing AGS with the data, parameters to be controlled are as below:

// - Cloud frequency bands: 24 bands that represents hours of a day
//    e.g.) 0 to 1 → 1st band, 1 to 2 → 2nd band, …, 23 to 0 → 24th band
//    each band has high and low boundaries as a midi note value.
// - Cloud duration: a value between 100ms to 500ms
// - Grain density: hourly use duration rescaled up to 100 grains per second
// - Grain duration: a value between 10ms to 50ms
// - Amplitude envelope type: linear ADSR or bell-shaped Gaussian curve (hann window)
// - Waveform of a grain: a synthetic type among sine, saw, square, triangle, and impulse

// Author: Sihwa Park (sihwapark@ucsb.edu)
// 2018-03-20

// Copyright (C) 2018 Sihwa Park

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <mutex>
#include "AudioPlatform/AudioVisual.h"
#include "AudioPlatform/FFT.h"
#include "AudioPlatform/Synths.h"
#include "AudioPlatform/SoundDisplay.h"
#include <set>
#include <fstream>
#include <sstream>
#include <time.h>

using namespace ap;
using namespace std;

#define NUM_GRAINS (100)

template <typename Out>
void split(const string& s, char delim, Out result) {
  stringstream ss(s);
  string item;
  while(getline(ss, item, delim)) {
    *(result++) = item;
  }
}

vector<string> split(const string& s, char delim) {
  vector<string> elems;
  split(s, delim, back_inserter(elems));
  return elems;
}


Array hannWindow;


struct Grain {

  float grainDuration; // milliseconds
  unsigned grainDurationInSamples; // milliseconds
  float frequency;
  float frequnecyRatio;
  float minFrequency, maxFrequency;

  Sine sine = Sine(4096);
  MultiSynth synth;

  Line attack, decay;
  int state;
  float startTimeRatio;
  
  int waveFormType = 0;
  int envlopeType = 1;
  unsigned currentPosInSamples = 0;

  Grain(float s, float minFreq, float maxFreq, float freqRatio, float duration) {
    frequnecyRatio = freqRatio;
    minFrequency = minFreq;
    maxFrequency = maxFreq;
    frequency = minFrequency + freqRatio * (maxFrequency - minFrequency);

    grainDuration = duration;
    grainDurationInSamples = (duration / 1000.0f) * 44100.0f;

    startTimeRatio = s;

    sine.frequency(frequency);
    synth.frequency(frequency);

    reset();
  }
  void selectEnvelopeType(int t) {
    envlopeType = t;

    switch(waveFormType) {
        case 0:

          break;
        case 1: // saw
          synth.type = waveFormType - 1;
          break;
        default:
          break;
    }
  }

  void selectWaveformType(int t) {
    waveFormType = t;
  }

  float operator()() { return nextValue(); }

  float nextValue() {
    switch(state) {
    case 0:
      if(attack.done()) state = 1;
      break;
    case 1:
      if(decay.done()) state = 2;
      break;
    case 2:
      break;
    default:
      break;
    }

    float v = 0;
    if(hasNext()) {
      switch(waveFormType) {
        case 0:
          v = sine();
          break;
        case 1: // saw
        case 2: // triangle;
        case 3: // square;
        case 4: // impulse;
          v = synth();
          break;
        default:
          break;
      }
      
      float e = 0;
      if(state == 0) e = attack();
      else if(state == 1) e = decay();

      float i = hannWindow.size * currentPosInSamples / (float)grainDurationInSamples;
      v *= (envlopeType == 0) ? e : hannWindow.get(i);

      //v *= e;

      currentPosInSamples++;
    }

    return v;
  }

  float hasNext() {
    return (currentPosInSamples < grainDurationInSamples);
  }

  void reset() {
    state = 0;
    attack.set(0, 1.0, grainDuration / 2.0);
    decay.set(1.0, 0, grainDuration / 2.0);
    currentPosInSamples = 0;
  }

  void resetDuation(float duration) {
    grainDuration = duration;

    reset();
  }

  void resetFrequencyBand(float minFreq, float maxFreq) {
    minFrequency = minFreq;
    maxFrequency = maxFreq;
    frequency = minFrequency + frequnecyRatio * (maxFrequency - minFrequency);

    sine.frequency(frequency);
    synth.frequency(frequency);
  }

};

struct Cloud {
  vector<Grain*> grains;

  std::set<Grain*> playList;

  unsigned hopSize;
  float minFrequency;
  float maxFrequency;
  float minMidi;
  float maxMidi;

  float grainDensity;
  float grainDuration; // milliseconds
  float cloudDuration; // milliseconds
  
  float increment;
  float time;
  unsigned grainIndex = 0;
  unsigned cloudSampleIndex;
  unsigned cloudDurationInSamples;
  int grainWaveFormType = 0;
  int grainEnvType = 0;

  void reset() {
    playList.clear();
    
    for(auto g : grains) {
      g->reset();
      //playList.insert(g);
    }
    grainIndex = 0;
    // time = 0;
    // grainTimer = 0;
    cloudSampleIndex = 0;

  }
  
  bool hasNext() {
    return (cloudSampleIndex < cloudDurationInSamples);
  }

  void setGrains(float density, float midiLow, float midiHigh, float gDuration, float duration) {
    // as a cumulus cloud, grains are randomly scattered whithin a given frequency band
    minMidi = midiLow;
    maxMidi = midiHigh;
    minFrequency = mtof(minMidi);
    maxFrequency = mtof(maxMidi);
    grainDensity = density;
    cloudDuration = duration;
    unsigned grainSize = grainDensity * (duration / 1000.0f);
    grainDuration = gDuration;

    
    cloudDurationInSamples = (duration / 1000.0f) * 44100.0f;
    cloudSampleIndex = 0;
    
    float maxStartTimeRatio = (cloudDuration - grainDuration) / cloudDuration;

    for(unsigned i = 0; i < grainSize; i++) {
      float freqRatio = (rand() / (double)RAND_MAX);
      float startTimeRatio = (rand() / (double)RAND_MAX) * maxStartTimeRatio;

      grains.push_back(new Grain(startTimeRatio, minFrequency, maxFrequency, freqRatio, grainDuration));
    }

    sort(grains.begin(), grains.end(), [](const Grain* a, const Grain* b) {
      return a->startTimeRatio < b->startTimeRatio;
    });
    
  }
  
  void selectWaveformType(int type) {
    grainWaveFormType = type;

    for(auto g : grains)
      g->selectWaveformType(grainWaveFormType);
  }

  void selectEnvelopeType(int type) {
    grainEnvType = type;

    for(auto g : grains)
      g->selectEnvelopeType(grainEnvType); 
  }

  void resetFrequencyBand(float midiLow, float midiHigh) {
    minMidi = midiLow;
    maxMidi = midiHigh;
    minFrequency = mtof(minMidi);
    maxFrequency = mtof(maxMidi);

    for(auto g : grains)
      g->resetFrequencyBand(minFrequency, maxFrequency);
  }

  void resetCloudDuration(float duration) {
    //printf("grainSize: %d\n", grains.size());

    cloudDuration = duration;
    unsigned grainSize = grainDensity * (duration / 1000.0f);

    cloudDurationInSamples = (duration / 1000.0f) * 44100.0f;

    if(cloudSampleIndex > cloudDurationInSamples)
      cloudSampleIndex = cloudDurationInSamples;

    int diff = grainSize - grains.size();
    
    if(diff < 0) {
      if(grainIndex > grainSize) grainIndex = 0;
      
      grains.erase(grains.end() + diff, grains.end());

    } else if(diff > 0) {
      float lastValue = (grains.size() > 0) ? (grains[grains.size() - 1])->startTimeRatio : 0;
      float maxStartTimeRatio = (cloudDuration - grainDuration) / cloudDuration;

      for(int i = 0; i < diff; i++) {
        float freqRatio = (rand() / (double)RAND_MAX);
        
        float startTimeRatio = lastValue + (maxStartTimeRatio - lastValue) * (rand() / (double)RAND_MAX);
        grains.push_back(new Grain(startTimeRatio, minFrequency, maxFrequency, freqRatio, grainDuration));
      }

      sort(grains.end() - diff, grains.end(), [](const Grain* a, const Grain* b) {
        return a->startTimeRatio < b->startTimeRatio;
      });
    }  

    reset();
  }

  void resetGrainDuration(float duration) {
    grainDuration = duration;

    for(auto g : grains)
      g->resetDuation(grainDuration);
  }

  float operator()() { return nextValue(); }
  
  float nextValue() {
    
    //time += increment;
    
    // if(time >= cloudDuration / 1000.0f) {
    //   time = cloudDuration / 1000.0f;
    //   // cloud duration expired
    // }
    if(grainIndex < grains.size() && cloudSampleIndex >= grains[grainIndex]->startTimeRatio * cloudDurationInSamples) {
      playList.insert(grains[grainIndex]);
      grainIndex++;
    }
    
    // grainTimer += grainTimerInc;
    // if(grainTimer >= 1) {
    //   grainTimer = 0;

    //   if(grainIndex < grains.size()) {
    //     playList.insert(grains[grainIndex]);
    //     grainIndex++;
    //   }
    // }

    float v = 0;
    set<Grain*> shouldRemove;

    int count = 0;
    for(auto g : playList) {
      if (g->hasNext()) {
        v += g->nextValue();
        count++;
      }
      else
        shouldRemove.insert(g);
    }

    if(count > 0) v /= (float)count;

    for(auto g : shouldRemove) playList.erase(g);


    cloudSampleIndex++;

    if(cloudSampleIndex >= cloudDurationInSamples)
      cloudSampleIndex = cloudDurationInSamples;
    
    return v;
  }
};

ImVec2 addVectors(ImVec2 &a, ImVec2 &b) {
  return ImVec2(a.x + b.x, a.y + b.y);
}

struct App : AudioVisual {
  
  SamplePlayer player;
  Line gain;
  
  SoundDisplay display;
  vector<Cloud*> clouds;
  vector<vector<float>> allData;

  float midiLimit = ftom(sampleRate * 0.5);

  bool play = false;

  float cloudDuration = 200.0f;
  unsigned cloudDurationInSamples = (cloudDuration / 1000.0f) * 44100.0f;
  float grainDuration = 20.0f;

  unsigned days = 0;
  unsigned currentPosInSamples = 0;
  unsigned elapsedDay = 0;
  
  float freqBands[24][2];
  bool mute[24], solo[24];
  int grainWaveFormType = 0;
  int grainEnvType = 0;

  void setup() {
    
    display.setup(4 * blockSize);
    hann(hannWindow, 4096);

    float minFrequency = 400.0f;
    float maxFrequency = 10000.0f;
    
    float maxMidi = ftom(maxFrequency);
    float minMidi = ftom(minFrequency);
  
    float freqBandPadding = 1;
    float freqBandwidth = (maxMidi - minMidi - freqBandPadding * 23.0f) / 24.0f;
    
    for(unsigned i = 0; i < 24; i++) {
      mute[i] = solo[i] = false;

      float midiLow = minMidi + (freqBandwidth + freqBandPadding) * i;
      float midiHigh =  midiLow + freqBandwidth;

      freqBands[i][0] = midiLow;
      freqBands[i][1] = midiHigh;
    }

    loadPreset();

    ifstream file;
    file.open("final/hourlyLength.txt");

    if(file.is_open() == false) {
      printf("Error: can't open final/hourlyLength.txt file!\n");
      exit(1);
    }

    string line;
    srand(time(NULL));

    while(getline(file, line)) {
      vector<string> data = split(line, ':');
      unsigned freqBandIndex = 0;
      vector<float> hourlyData;

      for(auto& s : split(data[1], ' ')) {
        float useLength = stof(s);
        float grainDensity = useLength * 100.0f / 60.0f;

        Cloud *cloud = new Cloud;
        cloud->setGrains((int)grainDensity, 
          freqBands[freqBandIndex][0], freqBands[freqBandIndex][1], grainDuration, cloudDuration);

        freqBandIndex++;
        clouds.push_back(cloud);
        hourlyData.push_back(useLength);  
      }

      allData.push_back(hourlyData);

      days++;
    }

    file.close();
  }

  void loadPreset() {
    ifstream file;
    file.open("final/setting.txt");
    string line;
    if(file.is_open() == false) {
      printf("Error: setting.txt does not exist!\n");
    } else {

      getline (file,line);
      cloudDuration = stof(line);

      cloudDurationInSamples = (cloudDuration / 1000.0f) * 44100.0f;

      getline (file,line);
      grainDuration = stof(line);

      for(int i = 0; i < 24; i++) {
        getline (file,line);
        
        vector<string> midi = split(line, ' ');
        freqBands[i][0] = stof(midi[0]);
        freqBands[i][1] = stof(midi[1]);
      }

      getline (file,line);
      grainWaveFormType = stoi(line);

      getline (file,line);
      grainEnvType = stoi(line);

      file.close();
    }      
  }
  void audio(float* out) {
    
    for (unsigned i = 0; i < blockSize * channelCount; i += channelCount) {
      float f = 0;
      
      if(play == true && clouds.size() > 0) {
        
        bool dayDone = true;
        for(unsigned j = elapsedDay * 24; j < (elapsedDay + 1) * 24; j++) {
          unsigned hour = j - elapsedDay * 24;

          if(clouds[j]->cloudDuration != cloudDuration) 
            clouds[j]->resetCloudDuration(cloudDuration);
          if(clouds[j]->grainDuration != grainDuration) 
            clouds[j]->resetGrainDuration(grainDuration);

          if(clouds[j]->minMidi != freqBands[hour][0] 
            || clouds[j]->maxMidi != freqBands[hour][1]) {
            clouds[j]->resetFrequencyBand(freqBands[hour][0], freqBands[hour][1]);
          }

          if(clouds[j]->grainWaveFormType != grainWaveFormType)
            clouds[j]->selectWaveformType(grainWaveFormType);

          if(clouds[j]->grainEnvType != grainEnvType)
            clouds[j]->selectEnvelopeType(grainEnvType);

          float v = clouds[j]->nextValue();
          f += (mute[hour])? 0 : v; 

          bool hasNext = clouds[j]->hasNext();
          if(!hasNext) {
            clouds[j]->reset();
            //clouds[j]->resetCloudDuration(cloudDuration);
          } 
          dayDone &= !hasNext;
        }

        f /= (float)24;
        currentPosInSamples++;

        if(dayDone) {
          printf("day %d done\n", elapsedDay);
          elapsedDay++;
          
          if(elapsedDay == 365) {
            elapsedDay = 0;  
          }
        }
      }

      out[i + 1] = out[i + 0] = f * gain();
      
      
      display(f);
    }
  }

  void visual() {
    {
      int windowWidth, windowHeight;
      glfwGetWindowSize(window, &windowWidth, &windowHeight);
      

      ImGui::SetWindowPos("window", ImVec2(0, 0));
      ImGui::SetWindowSize("window", ImVec2(windowWidth, windowHeight));
      ImGui::Begin("window", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar);

      ImGui::Columns(2, NULL, false);
      static int zoom = 1;
      static unsigned lastDay = 0;

      ImGui::Text("Grain Spectrogram");
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 4.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));


      ImVec2 canvas_pos_top_left = ImGui::GetCursorScreenPos();
      ImVec2 canvas_size = ImGui::GetContentRegionAvail(); 
      
      canvas_size.y *= 0.5;
      canvas_size.y -= 20;

      ImVec2 canvas_pos_bottom_right = addVectors(canvas_pos_top_left, canvas_size);
      canvas_pos_bottom_right.y -= 20;
      ImGui::BeginChild("Grains", canvas_size, false, ImGuiWindowFlags_HorizontalScrollbar);
      


      ImDrawList* drawList = ImGui::GetWindowDrawList();

      drawList->AddRectFilled(canvas_pos_top_left, canvas_pos_bottom_right, ImGui::GetColorU32(ImGuiCol_FrameBg));
      drawList->AddLine(canvas_pos_top_left, ImVec2(canvas_pos_top_left.x, canvas_pos_bottom_right.y), ImColor(255, 255, 255));
      drawList->AddLine(ImVec2(canvas_pos_top_left.x, canvas_pos_bottom_right.y), canvas_pos_bottom_right, ImColor(255, 255, 255));
      
      float unitDayWidth = floor(canvas_size.x / (365.0 / (zoom + 1)));
      float ratio = currentPosInSamples / (float)(cloudDurationInSamples);
      //printf("day: %d, samples: %d, %f\n", elapsedDay, currentPosInSamples, ratio);

      for(unsigned i = 0; i < days; i++) {
        ImGui::SameLine();
        ImGui::BeginChild(ImGui::GetID((void*)(intptr_t)i), ImVec2(unitDayWidth, canvas_size.y - 20), false);
        ImVec2 pos_top_left = ImGui::GetCursorScreenPos();
        ImVec2 size = ImGui::GetContentRegionAvail();
        ImVec2 pos_bottom_right = addVectors(pos_top_left, size);
        
        ImDrawList* draw_list2 = ImGui::GetWindowDrawList();
        draw_list2->AddRect(pos_top_left, pos_bottom_right, ImColor(200, 200, 200, 10));
        
        for(unsigned j = i * 24; j < (i + 1) * 24; j++) {
          
          for(auto g: clouds[j]->grains) {
            
            float y = pos_bottom_right.y - (g->frequency / (sampleRate * 0.5)) * size.y;
            
            float xStart = pos_top_left.x + g->startTimeRatio * size.x;
            float xEnd = xStart + (g->grainDuration / clouds[j]->cloudDuration) * size.x;

            draw_list2->AddLine(ImVec2(xStart, y), ImVec2(xEnd, y), ImColor(255, 0, 0));          
          }
        }

        if(i == lastDay) {
          float posX = pos_top_left.x + size.x * ratio;
          draw_list2->AddLine(ImVec2(posX, pos_top_left.y) , ImVec2(posX, pos_bottom_right.y), ImColor(255, 0, 0));
        }
        ImGui::EndChild();
      }
      
      float cursorX = (lastDay + ratio) * unitDayWidth;
      float currentPageEnd = ImGui::GetScrollX() + canvas_size.x;

      if(play && cursorX > currentPageEnd) {
        float page = floor(cursorX / canvas_size.x);
        ImGui::SetScrollX(page * canvas_size.x);
      } 

      if(play && elapsedDay == 0) {
        ImGui::SetScrollX(0);
      }

      float scrollX = ImGui::GetScrollX();

      ImGui::EndChild();
      
      ImGui::Text("Hourly Heatmap Data Visualization");
      ImGui::BeginChild("Heatmap", canvas_size, true, ImGuiWindowFlags_NoScrollbar);
      ImVec2 heatmap_pos_top_left = ImGui::GetCursorScreenPos();
      ImVec2 heatmap_size = ImGui::GetContentRegionAvail();
      ImVec2 heatmap_pos_bottom_right = addVectors(heatmap_pos_top_left, heatmap_size);
      heatmap_pos_bottom_right.y -= 0;
      ImDrawList* heatmapDrawList = ImGui::GetWindowDrawList();

      heatmapDrawList->AddRectFilled(heatmap_pos_top_left, heatmap_pos_bottom_right, ImGui::GetColorU32(ImGuiCol_FrameBg));

      for(unsigned i = 0; i < days; i++) {
        ImGui::SameLine();
        ImGui::BeginChild(ImGui::GetID((void*)(intptr_t)(i + days)), ImVec2(unitDayWidth, heatmap_size.y - 0), false);
        ImVec2 pos_top_left = ImGui::GetCursorScreenPos();
        ImVec2 size = ImGui::GetContentRegionAvail();
        ImVec2 pos_bottom_right = addVectors(pos_top_left, size);

        ImDrawList* draw_list2 = ImGui::GetWindowDrawList();
        draw_list2->AddRect(pos_top_left, pos_bottom_right, ImColor(200, 200, 200, 10));
        
        ImVec2 rect_size = ImVec2(size.x, size.y / 24.0f);
        vector<float> day = allData[i];

        
        for(int j = 0; j < 24; j++) {

          ImVec2 rect_top_left = ImVec2(pos_top_left.x, pos_top_left.y + (1 - (j + 1) / 24.0f) * size.y);
          ImVec2 rect_bottom_right = addVectors(rect_top_left, rect_size);
          int r = 244 * day[j] / 60.0;
          int g = 200 * day[j] / 60.0;
          int b = 10 * day[j] / 60.0;;
          int a = 255;
          draw_list2->AddRectFilled(rect_top_left, rect_bottom_right, ImColor(r, g, b, a));
        }

        if(i == lastDay) {
          float posX = pos_top_left.x + size.x * ratio;
          draw_list2->AddLine(ImVec2(posX, pos_top_left.y) , ImVec2(posX, pos_bottom_right.y), ImColor(255, 0, 0));
        }

        ImGui::EndChild();
      }

      ImGui::SetScrollX(scrollX);
      
      ImGui::EndChild();


      ImGui::PopStyleVar();
      ImGui::PopStyleVar();
      
      if (ImGui::Button("Play")) {
        play = true;
      } 
      
      ImGui::SameLine();
      if (ImGui::Button("Pause")) {
        play = false;
      }

      ImGui::SameLine();
      if (ImGui::Button("Stop")) {
        play = false;
        elapsedDay = 0;
        lastDay = 0;
        currentPosInSamples = 0;

        for(auto cloud: clouds) {
          cloud->reset();
        }
      } 

      ImGui::SameLine();
      if (ImGui::Button("Save")) {
        ofstream file;
        file.open("final/setting.txt");

        if(file.is_open() == false) {
          printf("Error: can't open file!\n");
        } else {

          
          file << cloudDuration << endl;
          file << grainDuration << endl;

          for(int i = 0 ; i < 24; i++) {
            file << freqBands[i][0] << " " << freqBands[i][1] << endl;
          }

          file << grainWaveFormType << endl;
          file << grainEnvType << endl;

          file.close();
        }
      }

      ImGui::SameLine();
      if (ImGui::Button("Load")) {
        loadPreset();
      }

      ImGui::SameLine();
      ImGui::PushItemWidth(canvas_size.x * 0.55);     
      ImGui::SliderInt("Zoom", &zoom, 1, 100);
      
      ImGui::NextColumn();
      
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 4.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

      const ImVec2 slider_size(20, canvas_size.y * 0.5 - 10);

      ImGui::Text("Hourly Frequecy Bands (midi note)");
      for(int i = 0; i < 24; i++) {
        if (i > 0) ImGui::SameLine();

        ImGui::BeginGroup();
        ImGui::PushID(i * 2);
        
        ImGui::VSliderFloat("##v", slider_size, &freqBands[i][1], 0, midiLimit, "");

        if(freqBands[i][1]  <= freqBands[i][0] + 1)
          freqBands[i][1] = freqBands[i][0] + 1;

        if (ImGui::IsItemActive() || ImGui::IsItemHovered())
                        ImGui::SetTooltip("%d High\n%.2f", i, freqBands[i][1]);
        ImGui::PopID();


        ImGui::PushID(i * 2 + 1);
        
        ImGui::VSliderFloat("##v", slider_size, &freqBands[i][0], 0, midiLimit, "");
        
        if(freqBands[i][0]  >= freqBands[i][1] - 1)
          freqBands[i][0] = freqBands[i][1] - 1;

        if (ImGui::IsItemActive() || ImGui::IsItemHovered())
                        ImGui::SetTooltip("%d Low\n%.2f", i, freqBands[i][0]);

        ImGui::PopID();

        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button, (mute[i])? (ImVec4)ImColor(255, 0, 0, 40) : (ImVec4)ImColor(125, 125, 125, 40));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (mute[i])? (ImVec4)ImColor(255, 0, 0, 125) : (ImVec4)ImColor(125, 125, 125, 125));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, (mute[i])? (ImVec4)ImColor(255, 0, 0, 255) : (ImVec4)ImColor(125, 125, 125, 255));
        
        if(ImGui::Button("M ")) {
          mute[i] = !mute[i];
        }
        ImGui::PopStyleColor(3);
        ImGui::PopID();

        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor(0, 255, 0, 40));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, (ImVec4)ImColor(0, 255, 0, 125));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, (ImVec4)ImColor(0, 255, 0, 255));
        if(ImGui::Button("S ")) {
          solo[i] = !solo[i];

          for(int j = 0; j < 24; j++) {
            mute[j] = solo[i];
            if(i != j) solo[j] = false;
          }

          if(solo[i]) mute[i] = !solo[i];
        }
        ImGui::PopStyleColor(3);
        ImGui::PopID();

        ImGui::EndGroup();
      }

      ImGui::PopStyleVar();
      ImGui::PopStyleVar();
      
      display.m.lock();
      ImGui::PushItemWidth(canvas_size.x * 0.7);     
      ImGui::PlotLines("Waveform", &display.history[0], display.history.size(), 0, "", FLT_MAX,
                     FLT_MAX, ImVec2(0, 50));
      
      float copy[display.history.size()];
      memcpy(copy, &display.history[0], display.history.size() * sizeof(float));
      
      for (unsigned i = 0; i < display.history.size(); ++i) copy[i] *= display.hann_window[i];
      display.fft.forward(&copy[0]);

      display.m.unlock();

      // convert to dB scale on the y axis
      for (auto& f : display.fft.magnitude) f = atodb(f);

      // draw the spectrum, linear in frequency
      ImGui::PlotLines("Spectrum", &display.fft.magnitude[0], display.fft.magnitude.size(), 0, "",
                       FLT_MAX, FLT_MAX, ImVec2(0, 50));

      // make a slider for "volume" level
      static float db = -60.0f;
      ImGui::SliderFloat("Level (dB)", &db, -60.0f, 3.0f);
      gain.set(dbtoa(db), 50.0f);

      float lastDuration = cloudDuration;
      ImGui::SliderFloat("Cloud Duration", &lastDuration, 100, 500);
      if(lastDuration != cloudDuration) {
        
        cloudDuration = lastDuration;
        cloudDurationInSamples = (cloudDuration / 1000.0f) * 44100.0f;

        if(play == false)
          for(auto c : clouds)
            c->resetCloudDuration(cloudDuration);
      }

      lastDuration = grainDuration;
      ImGui::SliderFloat("Grain Duration", &lastDuration, 10, 50);
      if(lastDuration != grainDuration) {
        
        grainDuration = lastDuration;

        if(play == false)
          for(auto c : clouds)
            c->resetGrainDuration(grainDuration);
      }

      static const char* types[] = { "Sine", "Saw", "Traingle", "Square", "Impulse" };
      
      int lastType = grainWaveFormType;
      ImGui::Combo("Grain Waveform", &lastType, types, IM_ARRAYSIZE(types));   // Combo using proper array. You can also pass a callback to retrieve array value, no need to create/copy an array just for that.
      
      if(lastType != grainWaveFormType) {
        grainWaveFormType = lastType;

        if(play == false)
          for(auto c : clouds)
            c->selectWaveformType(grainWaveFormType);
      }

      static const char* envTypes[] = { "Attack-Decay", "Hann Window" };
      
      int lastEnvType = grainEnvType;
      ImGui::Combo("Grain Envelope", &lastEnvType, envTypes, IM_ARRAYSIZE(envTypes));   // Combo using proper array. You can also pass a callback to retrieve array value, no need to create/copy an array just for that.
      
      if(lastEnvType != grainEnvType) {
        grainEnvType = lastEnvType;

        if(play == false)
          for(auto c : clouds)
            c->selectEnvelopeType(grainEnvType);
      }
      // if (ImGui::IsItemHovered() && lastType == 0) {
      //     ImGui::BeginTooltip();
      //     ImGui::Text("I am a fancy tooltip");
      //     static float arr[] = { 0.6f, 0.1f, 1.0f, 0.5f, 0.92f, 0.1f, 0.2f };
      //     ImGui::PlotLines("Curve", arr, IM_ARRAYSIZE(arr));
      //     ImGui::EndTooltip();
      // }

      
      //ImGui::ShowTestWindow();
      
      
      if(elapsedDay != lastDay) {
        lastDay = elapsedDay;
        currentPosInSamples = 0;
      }
      

      ImGui::End();
    }
  }
};

int main() { App().start(); }
