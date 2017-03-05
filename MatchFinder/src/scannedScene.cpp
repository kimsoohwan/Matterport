
#include "stdafx.h"

#include "scannedScene.h"
#include "imageHelper.h"
#include "normals.h"
#include "globalAppState.h"

#include "mLibFLANN.h"
#include "omp.h"


void ScannedScene::findKeyPoints()
{
	m_keyPoints.clear();
	const float depthSigmaD = GAS::get().s_depthFilterSigmaD;
	const float depthSigmaR = GAS::get().s_depthFilterSigmaR;

	for (size_t sensorIdx = 0; sensorIdx < m_sds.size(); sensorIdx++) {
		SensorData* sd = m_sds[sensorIdx];
		const mat4f intrinsicInv = sd->m_calibrationDepth.m_intrinsic.getInverse();

		for (size_t imageIdx = 0; imageIdx < sd->m_frames.size(); imageIdx++) {
			ColorImageR8G8B8 c = sd->computeColorImage(imageIdx);
			DepthImage32 d = sd->computeDepthImage(imageIdx);
			DepthImage32 dfilt = d; ImageHelper::bilateralFilter(dfilt, depthSigmaD, depthSigmaR);
			PointImage normalImage = SensorData::computeNormals(intrinsicInv, dfilt);
			//{ //debugging
			//	ColorImageR32G32B32 visNormalImage = normalImage;
			//	for (auto& p : visNormalImage) p.value = (p.value + 1.0f) / 0.5f; //scale [-1,1] to [0,1]
			//	FreeImageWrapper::saveImage("_depth.png", ColorImageR32G32B32(dfilt));
			//	FreeImageWrapper::saveImage("_normal.png", visNormalImage);
			//	const mat4f& camToWorld = sd->m_frames[imageIdx].getCameraToWorld();
			//	PointCloudf pc, pcw;
			//	for (const auto& p : d) {
			//		const vec3f& n = normalImage(p.x, p.y);
			//		if (n.x != -std::numeric_limits<float>::infinity()) {
			//			const vec3f c = intrinsicInv*vec3f(p.x*p.value, p.y*p.value, p.value);
			//			pc.m_points.push_back(c);
			//			pc.m_normals.push_back(n);
			//			pcw.m_points.push_back(camToWorld * c);
			//			pcw.m_normals.push_back((camToWorld.getRotation() * n).getNormalized());
			//		}
			//	}
			//	PointCloudIOf::saveToFile("_pcCam.ply", pc);
			//	PointCloudIOf::saveToFile("_pcWorld.ply", pcw);
			//	std::cout << "waiting..." << std::endl;
			//	getchar();
			//} //debugging

			//float sigmaD = 2.0f;	
			//float sigmaR = 0.1f;
			//sigmaD = 10.0f;
			//sigmaR = 10.0f;
			//FreeImageWrapper::saveImage("_before.png", c);
			//ImageHelper::bilateralFilter(c, sigmaD, sigmaR);
			//FreeImageWrapper::saveImage("_after.png", c);
			//std::cout << "here" << std::endl;
			//getchar();

			const mat4f& camToWorld = sd->m_frames[imageIdx].getCameraToWorld();

			const unsigned int maxNumKeyPoints = 512;
			const float minResponse = GAS::get().s_responseThresh;
			std::vector<KeyPoint> rawKeyPoints = KeyPointFinder::findKeyPoints(vec2ui(sensorIdx, imageIdx), c, maxNumKeyPoints, minResponse);

			MeshDataf md;
			size_t validKeyPoints = 0;
			for (KeyPoint& rawkp : rawKeyPoints) {
				const unsigned int padding = 50;	//don't take keypoints in the padding region of the image
				vec2ui loc = math::round(rawkp.m_pixelPos);
				if (d.isValid(loc) && d.isValidCoordinate(loc + padding) && d.isValidCoordinate(loc - padding)) {
					KeyPoint kp = rawkp;
					kp.m_depth = d(loc);
					kp.m_imageIdx = (unsigned int)imageIdx;
					kp.m_sensorIdx = (unsigned int)sensorIdx;
					////kp.m_pixelPos = vec2f(rawkp.x, rawkp.y);
					kp.m_pixelPos = vec2f(loc);
					//kp.m_size = rawkp.m_size;
					//kp.m_angle = rawkp.m_angle;
					//kp.m_octave = rawkp.m_octave;
					//kp.m_scale = rawkp.m_scale;
					//kp.m_response = rawkp.m_response;
					//kp.m_opencvPackOctave = rawkp.m_opencvPackOctave;

					const vec3f cameraPos = (intrinsicInv*vec4f(kp.m_pixelPos.x*kp.m_depth, kp.m_pixelPos.y*kp.m_depth, kp.m_depth, 0.0f)).getVec3();
					kp.m_worldPos = camToWorld * cameraPos;

					const vec3f normal = normalImage(loc); 
					if (normal.x == -std::numeric_limits<float>::infinity()) continue;
					kp.m_worldNormal = (camToWorld.getRotation() * normal).getNormalized();

					validKeyPoints++;

					m_keyPoints.push_back(kp);

					if (imageIdx == 0) md.merge(Shapesf::sphere(0.01f, vec3f(kp.m_worldPos), 10, 10, vec4f(1.0f, 0.0f, 0.0f, 1.0f)).computeMeshData());
				}
			}

			std::cout << "\r" << "image: " << sensorIdx << "|" << imageIdx  << " found " << validKeyPoints << " keypoints";
			//if (imageIdx == 0) MeshIOf::saveToFile("test.ply", md);
			//if (imageIdx == 50) break;

			if (GAS::get().s_maxNumImages > 0 && imageIdx + 1 >= GAS::get().s_maxNumImages) break;
		}
	}
	std::cout << std::endl;
}

void ScannedScene::negativeKeyPoints()
{
	m_keyPointNegatives.clear();

	for (size_t i = 0; i < m_keyPointMatches.size(); i++) {
		KeyPointMatch noMatch;
		noMatch.m_kp0 = m_keyPointMatches[i].m_kp0;		//first key point is the same

		//search for non-matching keypoint in a different image.
		size_t tries = 0;
		for (;; tries++) {
			if (tries > 1000) throw MLIB_EXCEPTION("too many tries to find a negative match...");

			unsigned int idx = math::randomUniform(0u, (unsigned int)m_keyPoints.size() - 1);
			if (idx >= (unsigned int)m_keyPoints.size()) {
				std::cout << __FUNCTION__ << " ERROR: " << idx << "\t" << m_keyPoints.size() << std::endl;
			}
			noMatch.m_kp1 = m_keyPoints[idx];
			float dist = (noMatch.m_kp0.m_worldPos - noMatch.m_kp1.m_worldPos).length();
			if (noMatch.m_kp0.isSameImage(noMatch.m_kp1) || dist <= GAS::get().s_matchThresh) continue;	//invalid; either same image or within the threshold
			else {
				//found a good match
				noMatch.m_offset = vec2f(0.0f);
				m_keyPointNegatives.push_back(noMatch);
				break;
			}
		}
	}

	if (m_keyPointMatches.size() != m_keyPointNegatives.size()) throw MLIB_EXCEPTION("something went wrong here... positive and negative match counts should be the same");
}

void ScannedScene::matchKeyPoints()
{
	const float radius = GAS::get().s_matchThresh;
	const unsigned int maxK = 5;

	unsigned int currKeyPoint = 0;
	std::vector<std::vector<NearestNeighborSearchFLANNf*>> nns(m_sds.size());
	std::vector<std::vector<unsigned int>> nn_offsets(m_sds.size());
	for (size_t sensorIdx = 0; sensorIdx < nns.size(); sensorIdx++) {
		nns[sensorIdx].resize(m_sds[sensorIdx]->m_frames.size(), nullptr);
		nn_offsets[sensorIdx].resize(m_sds[sensorIdx]->m_frames.size(), 0);
		for (size_t frameIdx = 0; frameIdx < m_sds[sensorIdx]->m_frames.size(); frameIdx++) {

			nn_offsets[sensorIdx][frameIdx] = currKeyPoint;

			std::vector<float> points;
			for (;	currKeyPoint < (UINT)m_keyPoints.size() &&
				m_keyPoints[currKeyPoint].m_sensorIdx == sensorIdx &&
				m_keyPoints[currKeyPoint].m_imageIdx == frameIdx; currKeyPoint++) {
				points.push_back(m_keyPoints[currKeyPoint].m_worldPos.x);
				points.push_back(m_keyPoints[currKeyPoint].m_worldPos.y);
				points.push_back(m_keyPoints[currKeyPoint].m_worldPos.z);
			}

			if (points.size())	{
				nns[sensorIdx][frameIdx] = new NearestNeighborSearchFLANNf(4 * maxK, 1);
				nns[sensorIdx][frameIdx]->init(points.data(), (unsigned int)points.size() / 3, 3, maxK);
			}
		}
	}


	for (size_t keyPointIdx = 0; keyPointIdx < m_keyPoints.size(); keyPointIdx++) {

		if (keyPointIdx % 100 == 0)
			std::cout << "\rmatching keypoint " << keyPointIdx << " out of " << m_keyPoints.size();

		KeyPoint& kp = m_keyPoints[keyPointIdx];
		const float* query = (const float*)&kp.m_worldPos;

		const size_t sensorIdx = kp.m_sensorIdx;
		const size_t frameIdx = kp.m_imageIdx;

		for (size_t sensorIdx_dst = sensorIdx; sensorIdx_dst < nns.size(); sensorIdx_dst++) {

			size_t frameIdx_dst = 0;
			if (sensorIdx_dst == sensorIdx) frameIdx_dst = frameIdx + 1;

			for (; frameIdx_dst < nns[sensorIdx].size(); frameIdx_dst++) {

				auto* nn = nns[sensorIdx_dst][frameIdx_dst];
				if (nn == nullptr) continue;

				size_t numMatches = 0;
				std::vector<unsigned int> res = nn->fixedRadius(query, maxK, radius);
				auto resPair = nn->fixedRadiusDist(query, maxK, radius);
				auto resDist = nn->getDistances((UINT)res.size());
				for (size_t j = 0; j < res.size(); j++) {
					const KeyPoint& kp0 = m_keyPoints[keyPointIdx];
					const KeyPoint& kp1 = m_keyPoints[res[j] + nn_offsets[sensorIdx_dst][frameIdx_dst]];
					//check world normals
					float angleDist = std::acos(kp0.m_worldNormal | kp1.m_worldNormal);
					if (angleDist > 2.0f) continue; //ignore with world normals > ~115 degrees apart
					
					////debugging
					//const float degrees = math::radiansToDegrees(angleDist);
					//if (degrees > 90) { 
					//	KeyPointMatch m;
					//	m.m_kp0 = kp0;
					//	m.m_kp1 = kp1;
					//	MatchVisualization mv;
					//	mv.visulizeMatches(m_sds, std::vector<KeyPointMatch>(1, m), 10);
					//	m_sds[kp0.m_sensorIdx]->saveToPointCloud("frame0.ply", kp0.m_imageIdx);
					//	MeshDataf keymesh = Shapesf::sphere(0.03f, kp0.m_worldPos, 10, 10, vec4f(1.0f, 0.0f, 0.0f, 1.0f)).computeMeshData();
					//	keymesh.merge(Shapesf::cylinder(kp0.m_worldPos, kp0.m_worldPos + kp0.m_worldNormal * 0.2f, .02f, 10, 10, vec4f(1.0f, 0.0f, 0.0f, 1.0f)).computeMeshData());
					//	MeshIOf::saveToFile("frame0_key.ply", keymesh);
					//	m_sds[kp1.m_sensorIdx]->saveToPointCloud("frame1.ply", kp1.m_imageIdx);
					//	keymesh = Shapesf::sphere(0.03f, kp1.m_worldPos, 10, 10, vec4f(0.0f, 1.0f, 0.0f, 1.0f)).computeMeshData();
					//	keymesh.merge(Shapesf::cylinder(kp1.m_worldPos, kp1.m_worldPos + kp1.m_worldNormal * 0.2f, .02f, 10, 10, vec4f(0.0f, 1.0f, 0.0f, 1.0f)).computeMeshData());
					//	MeshIOf::saveToFile("frame1_key.ply", keymesh);
					//	int a = 5;
					//} //debugging

					KeyPointMatch m;
					m.m_kp0 = kp0;
					m.m_kp1 = kp1;

					mat4f worldToCam_kp1 = m_sds[m.m_kp1.m_sensorIdx]->m_frames[m.m_kp1.m_imageIdx].getCameraToWorld().getInverse();
					vec3f p = worldToCam_kp1 * m.m_kp0.m_worldPos;
					p = m_sds[m.m_kp1.m_sensorIdx]->m_calibrationDepth.cameraToProj(p);
					m.m_offset = m.m_kp1.m_pixelPos - vec2f(p.x, p.y);
					m_keyPointMatches.push_back(m);
					numMatches++;

					//std::cout << "orig: " << m.m_kp1.m_pixelPos << std::endl;
					//std::cout << "repr: " << vec2f(p.x, p.y) << std::endl;
					//std::cout << m.m_offset << std::endl;

					//std::cout << "match between: " << std::endl;
					//std::cout << m.m_kp0;
					//std::cout << m.m_kp1;

					//std::cout << "dist " << sensorIdx << ":\t" << (m.m_kp0.m_worldPos - m.m_kp1.m_worldPos).length() << std::endl;
					//std::cout << std::endl;
					//int a = 5;
				}
			}
		}
	}

	std::cout << std::endl;


	//clean up our mess...
	for (size_t sensorIdx = 0; sensorIdx < nns.size(); sensorIdx++) {
		for (size_t frameIdx = 0; frameIdx < nns[sensorIdx].size(); frameIdx++) {
			SAFE_DELETE(nns[sensorIdx][frameIdx]);
		}
	}


	std::cout << "Total matches found: " << m_keyPointMatches.size() << std::endl;
}

void ScannedScene::saveImages(const std::string& outPath) const
{
	if (!util::directoryExists(outPath)) {
		util::makeDirectory(outPath);
	}

	const bool bSaveNormals = !m_normals.empty();

	const unsigned int maxNumSensors = std::min((unsigned int)m_sds.size(), GAS::get().s_maxNumSensFiles);
	for (unsigned int sensorIdx = 0; sensorIdx < maxNumSensors; sensorIdx++) {
		SensorData* sd = m_sds[sensorIdx];

		for (size_t imageIdx = 0; imageIdx < sd->m_frames.size(); imageIdx++) {
			ColorImageR8G8B8 c = sd->computeColorImage(imageIdx);
			//DepthImage32 d = sd->computeDepthImage(imageIdx);
			//TODO depth image?

			char s[256];
			sprintf(s, "color-%02d-%06d.jpg", (UINT)sensorIdx, (UINT)imageIdx);
			const std::string outFileColor = outPath + "/" + std::string(s);

			std::cout << "\r" << outFileColor;
			FreeImageWrapper::saveImage(outFileColor, c);

			if (GAS::get().s_maxNumImages > 0 && imageIdx + 1 >= GAS::get().s_maxNumImages) break;
		}
		std::cout << std::endl;
	}
}

void ScannedScene::computeNormals(NORMAL_TYPE type)
{
	switch (type) 
	{
	case DEPTH_NORMALS:
		computeDepthNormals();
		break;
	case MESH_NORMALS:
		computeMeshNormals();
		break;
	default:
		//should never come here
		break;
	}
}

void ScannedScene::computeDepthNormals()
{
	std::cout << "computing depth normals... ";
	const unsigned int maxNumSensors = std::min((unsigned int)m_sds.size(), GAS::get().s_maxNumSensFiles);
	m_normals.resize(maxNumSensors);
	NormalExtractor extractor(GAS::get().s_outWidth, GAS::get().s_outHeight);
	for (unsigned int sensorIdx = 0; sensorIdx < maxNumSensors; sensorIdx++) {
		extractor.computeDepthNormals(m_sds[sensorIdx], GAS::get().s_depthFilterSigmaD, GAS::get().s_depthFilterSigmaR, m_normals[sensorIdx]);
	}
	std::cout << "done!" << std::endl;
}

void ScannedScene::computeMeshNormals()
{
	std::cout << "computing mesh normals... ";
	//load mesh
	const std::string meshFile = m_path + "/" + m_name + "_vh_clean.ply"; //highest-res
	TriMeshf mesh = TriMeshf(MeshIOf::loadFromFile(meshFile));
	mesh.computeNormals();
	const unsigned int maxNumSensors = std::min((unsigned int)m_sds.size(), GAS::get().s_maxNumSensFiles);
	m_normals.resize(maxNumSensors);
	std::vector<NormalExtractor*> extractors(omp_get_max_threads());
	for (unsigned int i = 0; i < extractors.size(); i++) extractors[i] = new NormalExtractor(GAS::get().s_outWidth, GAS::get().s_outHeight);
#pragma omp parallel for
	for (int sensorIdx = 0; sensorIdx < (int)maxNumSensors; sensorIdx++) {
		const int thread = omp_get_thread_num();
		NormalExtractor& extractor = *extractors[thread];
		extractor.computeMeshNormals(m_sds[sensorIdx], mesh, GAS::get().s_renderDepthMin, GAS::get().s_renderDepthMax, m_normals[sensorIdx]);
	}

	for (unsigned int i = 0; i < extractors.size(); i++)
		SAFE_DELETE(extractors[i]);
	std::cout << "done!" << std::endl;
}

void ScannedScene::saveNormalImages(const std::string& outPath) const
{
	if (m_normals.empty()) {
		std::cout << "no normals to save" << std::endl;
		return;
	}

	if (!util::directoryExists(outPath)) {
		util::makeDirectory(outPath);
	}

	const unsigned int maxNumSensors = std::min((unsigned int)m_sds.size(), GAS::get().s_maxNumSensFiles);
	for (unsigned int sensorIdx = 0; sensorIdx < maxNumSensors; sensorIdx++) {
		SensorData* sd = m_sds[sensorIdx];
		const mat4f depthIntrinsicInv = sd->m_calibrationDepth.m_intrinsic.getInverse();

		for (size_t imageIdx = 0; imageIdx < sd->m_frames.size(); imageIdx++) {

			char s[256];
			sprintf(s, "normal-%02d-%06d.png", (UINT)sensorIdx, (UINT)imageIdx);
			const std::string outFileNormal = outPath + "/" + std::string(s);

			std::cout << "\r" << outFileNormal;
			NormalExtractor::saveNormalImage(outFileNormal, m_normals[sensorIdx][imageIdx]);

			if (GAS::get().s_maxNumImages > 0 && imageIdx + 1 >= GAS::get().s_maxNumImages) break;
		}
		std::cout << std::endl;
	}
}
