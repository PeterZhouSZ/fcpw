namespace fcpw {

template<size_t DIM>
inline void loadInstanceTransforms(const std::string& filename,
								   std::vector<std::vector<Transform<DIM>>>& instanceTransforms)
{
	// load file
	std::ifstream in(filename);
	LOG_IF(FATAL, in.is_open() == false) << "Unable to open file: " << filename;

	// parse transforms
	std::string line;
	while (getline(in, line)) {
		std::stringstream ss(line);
		int object;
		ss >> object;

		int nTransform = (int)instanceTransforms[object].size();
		instanceTransforms[object].emplace_back(Transform<DIM>());

		for (int i = 0; i <= DIM; i++) {
			for (int j = 0; j <= DIM; j++) {
				ss >> instanceTransforms[object][nTransform].matrix()(i, j);
			}
		}
	}

	// close file
	in.close();
}

} // namespace fcpw
