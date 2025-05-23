#include "normalstask.h"
#include "../src/jpeg_decoder.h"
#include "../src/jpeg_encoder.h"
#include "../src/imageset.h"
#include "../src/relight_threadpool.h"
#include "../src/bni_normal_integration.h"
#include "../src/flatnormals.h"

#include <Eigen/Eigen>
#include <Eigen/Core>
#include <Eigen/Dense>

#include <QJsonArray>
#include <QJsonDocument>
#include <QDebug>
#include <QImage>
#include <vector>
#include <iostream>
#include <time.h>

using namespace std;
using namespace Eigen;

//////////////////////////////////////////////////////// NORMALS TASK //////////////////////////////////////////////////////////
/// \brief NormalsTask: Takes care of creating the normals from the images given in a folder (inputFolder) and saves the file
///         in the outputFolder. After applying the crop described in the QRect passed as an argument to the constructor,
///         the NormalsTask creates a NormalsWorker for each line in the final image.
///         That NormalsWorker fills a vector with the colors of the normals in that line.
///

void NormalsTask::run() {
	status = RUNNING;
	QList<QVariant> qlights = (*this)["lights"].value.toList();
	std::vector<Vector3f> lights(qlights.size()/3);
	for(int i = 0; i < qlights.size(); i+= 3)
		for(int k = 0; k < 3; k++)
			lights[i/3][k] = qlights[i+k].toDouble();


	ImageSet imageSet;
	imageSet.images = (*this)["images"].value.toStringList();

	imageSet.initImages(input_folder.toStdString().c_str());
	imageSet.initFromDome(project->dome);

	if(hasParameter("crop")) {
		QRect rect = (*this)["crop"].value.toRect();
		imageSet.crop(rect.left(), rect.top(), rect.width(), rect.height());
	}

	// Normals vector

	int start = clock();
	imageSet.setCallback(nullptr);

	std::vector<float> normals(imageSet.width * imageSet.height * 3);

	RelightThreadPool pool;
	PixelArray line;

	pool.start(QThread::idealThreadCount());

	for (int i=0; i<imageSet.height; i++) {
		// Read a line
		imageSet.readLine(line);

		// Create the normal task and get the run lambda
		uint32_t idx = i * 3 * imageSet.width;
		float* data = &normals[idx];

		NormalsWorker *task = new NormalsWorker(solver, i, line, data, imageSet);

		std::function<void(void)> run = [this, task](void)->void {
			task->run();
			delete task;
		};

		pool.queue(run);
		pool.waitForSpace();

		if(!progressed("Computing normals...", ((float)i / imageSet.height) * 100))
			break;
	}

	// Wait for the end of all the threads
	pool.finish();

	if(flatMethod != NONE) {
		switch(flatMethod) {
		case RADIAL:
			flattenRadialNormals(imageSet.width, imageSet.height, normals);
			break;
		case FOURIER:
			double sigma = imageSet.width*0.2;
			flattenFourierNormals(imageSet.width, imageSet.height, normals, sigma);
			break;
		}
	}


	std::vector<uint8_t> normalmap(imageSet.width * imageSet.height * 3);
	for(size_t i = 0; i < normals.size(); i++)
		normalmap[i] = floor(((normals[i] + 1.0f) / 2.0f) * 255);

	// Save the final result
	QImage img(normalmap.data(), imageSet.width, imageSet.height, imageSet.width*3, QImage::Format_RGB888);
    // Set spatial resolution if known. Need to convert as pixelSize stored in mm/pixel whereas QImage requires pixels/m
    if( pixelSize > 0 ) {
        int dotsPerMeter = round(1000.0/pixelSize);
        img.setDotsPerMeterX(dotsPerMeter);
        img.setDotsPerMeterY(dotsPerMeter);
    }
	img.save(output);

	std::function<bool(QString s, int d)> callback = [this](QString s, int n)->bool { return this->progressed(s, n); };

	if(exportSurface || exportDepthmap) {
		if(!progressed("Integrating normals...", 0))
			return;
		std::vector<float> z;
		bni_integrate(callback, imageSet.width, imageSet.height, normals, z, exportK);
		if(z.size() == 0) {
			error = "Failed to integrate normals";
			status = FAILED;
			return;
		}
		QString filename = output.left(output.size() -4) + ".ply";

		if(!progressed("Saving surface...", 99))
			return;
		if(exportSurface)
			savePly(filename, imageSet.width, imageSet.height, z);

		filename = output.left(output.size() -4) + ".tif";

		if(exportDepthmap)
			saveTiff(filename + ".tif", imageSet.width, imageSet.height, z);
	}
	int end = clock();
	qDebug() << "Time: " << ((double)(end - start) / CLOCKS_PER_SEC);
	progressed("Finished", 100);
}

/**
 *   \brief NormalsWorker: generates the normals for a given line in the image set, depending on the method specified when
 *          creating the Worker.
**/


void NormalsWorker::run()
{
	switch (solver)
	{
	// L2 solver
	case NORMALS_L2:
		solveL2();
		break;
		// SBL solver
	case NORMALS_SBL:
		solveSBL();
		break;
		// RPCA solver
	case NORMALS_RPCA:
		solveRPCA();
		break;
	}

	// Deallocate line (TODO: useless?)
	std::vector<Pixel>().swap(m_Row);
}


void NormalsWorker::solveL2()
{
	std::vector<Vector3f> &m_Lights = m_Imageset.lights();

	// Pixel data
	Eigen::MatrixXd mLights(m_Lights.size(), 3);
	Eigen::MatrixXd mPixel(m_Lights.size(), 1);
	Eigen::MatrixXd mNormals;

	unsigned int normalIdx = 0;


	// Fill the lights matrix
	for (size_t i = 0; i < m_Lights.size(); i++)
		for (int j = 0; j < 3; j++)
			mLights(i, j) = m_Lights[i][j];

	// For each pixel in the line solve the system
	//TODO do it in a single pass, it's faster.
	for (size_t p = 0; p < m_Row.size(); p++) {
		// Fill the pixel vector
		for (size_t m = 0; m < m_Lights.size(); m++)
			mPixel(m, 0) = m_Row[p][m].mean();

		if(m_Imageset.light3d) {
			for(size_t i = 0; i < m_Lights.size(); i++) {
				Vector3f light = m_Imageset.relativeLight(m_Lights[i], p, m_Imageset.height - row);
				light.normalize();
				for (int j = 0; j < 3; j++)
					mLights(i, j) = light[j];
			}
		}

		mNormals = (mLights.transpose() * mLights).ldlt().solve(mLights.transpose() * mPixel);
		mNormals.col(0).normalize();
		m_Normals[normalIdx+0] = mNormals(0, 0);
		m_Normals[normalIdx+1] = mNormals(1, 0);
		m_Normals[normalIdx+2] = mNormals(2, 0);

		normalIdx += 3;
	}
}

void NormalsWorker::solveSBL()
{
}

void NormalsWorker::solveRPCA()
{
}

